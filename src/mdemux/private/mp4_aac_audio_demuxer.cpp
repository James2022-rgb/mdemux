// TU header --------------------------------------------
#include "mdemux/public/mp4_aac_audio_demuxer.h"

// c++ system headers -----------------------------------
#include <cstring>

// public project headers -------------------------------
#include "mbase/public/assert.h"
#include "mbase/public/log.h"

// external headers -------------------------------------
#include "lsmash.h"

namespace mdemux {

struct Mp4AacAudioDemuxer::Impl {
  lsmash_root_t*    ls_root            = nullptr;
  uint32_t          audio_track_id     = 0;
  lsmash_summary_t* ls_summary         = nullptr;  // borrowed from ls_root
  uint32_t          total_sample_count = 0;
  uint32_t          timescale          = 0;
  uint32_t          sample_rate        = 0;
  uint32_t          channel_count      = 0;

  ~Impl() {
    if (ls_root != nullptr) {
      lsmash_destroy_root(ls_root);
      ls_root = nullptr;
    }
  }
};

Mp4AacAudioDemuxer::Mp4AacAudioDemuxer() : impl_(std::make_unique<Impl>()) {}
Mp4AacAudioDemuxer::~Mp4AacAudioDemuxer() = default;

std::unique_ptr<Mp4AacAudioDemuxer> Mp4AacAudioDemuxer::Open(std::string const& path) {
  lsmash_file_parameters_t ls_fp{};
  if (lsmash_open_file(path.c_str(), 1, &ls_fp) != 0) {
    MBASE_LOG_ERROR("Mp4AacAudioDemuxer: lsmash_open_file failed: {}", path);
    return nullptr;
  }

  lsmash_root_t* ls_root = lsmash_create_root();
  lsmash_file_t* ls_file = lsmash_set_file(ls_root, &ls_fp);
  lsmash_read_file(ls_file, &ls_fp);

  lsmash_movie_parameters_t ls_mp{};
  if (lsmash_get_movie_parameters(ls_root, &ls_mp) != 0) {
    MBASE_LOG_ERROR("Mp4AacAudioDemuxer: lsmash_get_movie_parameters failed: {}", path);
    lsmash_destroy_root(ls_root);
    return nullptr;
  }

  uint32_t audio_track_id = 0;
  uint32_t timescale      = 0;
  for (uint32_t track_n = 1; track_n <= ls_mp.number_of_tracks; ++track_n) {
    uint32_t const track_id = lsmash_get_track_ID(ls_root, track_n);
    lsmash_media_parameters_t ls_mdp{};
    MBASE_ASSERT(lsmash_get_media_parameters(ls_root, track_id, &ls_mdp) == 0);
    if (ls_mdp.handler_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK) {
      audio_track_id = track_id;
      timescale      = ls_mdp.timescale;
      break;
    }
  }
  if (audio_track_id == 0) {
    MBASE_LOG_ERROR("Mp4AacAudioDemuxer: no audio track in {}", path);
    lsmash_destroy_root(ls_root);
    return nullptr;
  }

  if (lsmash_construct_timeline(ls_root, audio_track_id) != 0) {
    MBASE_LOG_ERROR("Mp4AacAudioDemuxer: lsmash_construct_timeline failed for track {}", audio_track_id);
    lsmash_destroy_root(ls_root);
    return nullptr;
  }

  // Locate the AAC (mp4a) sample-entry summary.
  lsmash_summary_t* ls_summary = nullptr;
  uint32_t const summary_count = lsmash_count_summary(ls_root, audio_track_id);
  for (uint32_t i = 1; i <= summary_count; ++i) {
    lsmash_summary_t* s = lsmash_get_summary(ls_root, audio_track_id, i);
    if (s->sample_type.fourcc == ISOM_CODEC_TYPE_MP4A_AUDIO.fourcc) {
      ls_summary = s;
      break;
    }
  }
  if (ls_summary == nullptr) {
    MBASE_LOG_ERROR("Mp4AacAudioDemuxer: no MP4A summary in audio track {}", audio_track_id);
    lsmash_destroy_root(ls_root);
    return nullptr;
  }

  uint32_t const total_sample_count = lsmash_get_sample_count_in_media_timeline(ls_root, audio_track_id);

  auto const* audio_summary = reinterpret_cast<lsmash_audio_summary_t const*>(ls_summary);
  uint32_t const sample_rate   = audio_summary->frequency;
  uint32_t const channel_count = audio_summary->channels;

  auto demuxer = std::unique_ptr<Mp4AacAudioDemuxer>(new Mp4AacAudioDemuxer{});
  demuxer->impl_->ls_root            = ls_root;
  demuxer->impl_->audio_track_id     = audio_track_id;
  demuxer->impl_->ls_summary         = ls_summary;
  demuxer->impl_->total_sample_count = total_sample_count;
  demuxer->impl_->timescale          = timescale;
  demuxer->impl_->sample_rate        = sample_rate;
  demuxer->impl_->channel_count      = channel_count;
  return demuxer;
}

std::vector<uint8_t> Mp4AacAudioDemuxer::GetAscBytes() const {
  MBASE_ASSERT(impl_->ls_summary != nullptr);
  uint32_t const cs_count = lsmash_count_codec_specific_data(impl_->ls_summary);
  for (uint32_t i = 1; i <= cs_count; ++i) {
    lsmash_codec_specific_t* spec = lsmash_get_codec_specific_data(impl_->ls_summary, i);
    if (spec->type != LSMASH_CODEC_SPECIFIC_DATA_TYPE_MP4SYS_DECODER_CONFIG) continue;

    // The unstructured form is the wrapping ESDS DecoderConfigDescriptor;
    // the bytes fdk-aac wants are the inner DecoderSpecificInfo (= the
    // raw AudioSpecificConfig). Reach those through the structured
    // accessor.
    lsmash_codec_specific_t* structured =
      lsmash_convert_codec_specific_format(spec, LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED);
    MBASE_ASSERT(structured != nullptr);

    auto* params = static_cast<lsmash_mp4sys_decoder_parameters_t*>(structured->data.structured);
    MBASE_ASSERT(params != nullptr);

    uint8_t* dsi_payload     = nullptr;
    uint32_t dsi_payload_len = 0;
    int const rc = lsmash_get_mp4sys_decoder_specific_info(params, &dsi_payload, &dsi_payload_len);
    MBASE_ASSERT_MSG(rc == 0 && dsi_payload != nullptr && dsi_payload_len > 0,
      "lsmash_get_mp4sys_decoder_specific_info failed (no AudioSpecificConfig in esds).");

    std::vector<uint8_t> out(dsi_payload_len);
    std::memcpy(out.data(), dsi_payload, dsi_payload_len);
    // L-SMASH allocates dsi_payload via lsmash_malloc; free it.
    lsmash_free(dsi_payload);
    return out;
  }
  return {};
}

Mp4AacAudioDemuxer::Sample Mp4AacAudioDemuxer::GetSampleByDecodeNo(uint32_t sample_no) {
  lsmash_sample_t* sample = lsmash_get_sample_from_media_timeline(impl_->ls_root, impl_->audio_track_id, sample_no);
  MBASE_ASSERT(sample != nullptr);
  Sample out;
  out.data.assign(sample->data, sample->data + sample->length);
  lsmash_delete_sample(sample);
  return out;
}

uint32_t Mp4AacAudioDemuxer::total_sample_count() const { return impl_->total_sample_count; }
uint32_t Mp4AacAudioDemuxer::timescale()          const { return impl_->timescale; }
uint32_t Mp4AacAudioDemuxer::sample_rate()        const { return impl_->sample_rate; }
uint32_t Mp4AacAudioDemuxer::channel_count()      const { return impl_->channel_count; }

} // namespace mdemux
