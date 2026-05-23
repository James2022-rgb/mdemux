// TU header --------------------------------------------
#include "mdemux/public/mp4_gpmf_track_demuxer.h"

// public project headers -------------------------------
#include "mbase/public/assert.h"
#include "mbase/public/log.h"

// external headers -------------------------------------
#include "lsmash.h"

namespace mdemux {

namespace {

// GoPro Metadata Format sample-entry fourcc. L-SMASH has no named
// constant for it.
constexpr uint32_t kGpmdCodecFourcc = LSMASH_4CC('g', 'p', 'm', 'd');

} // namespace

struct Mp4GpmfTrackDemuxer::Impl {
  lsmash_root_t* ls_root            = nullptr;
  uint32_t       gpmd_track_id      = 0;
  uint32_t       total_sample_count = 0;
  uint32_t       timescale          = 0;

  ~Impl() {
    if (ls_root != nullptr) {
      lsmash_destroy_root(ls_root);
      ls_root = nullptr;
    }
  }
};

Mp4GpmfTrackDemuxer::Mp4GpmfTrackDemuxer() : impl_(std::make_unique<Impl>()) {}
Mp4GpmfTrackDemuxer::~Mp4GpmfTrackDemuxer() = default;

std::unique_ptr<Mp4GpmfTrackDemuxer> Mp4GpmfTrackDemuxer::Open(std::string const& path) {
  lsmash_file_parameters_t ls_fp{};
  if (lsmash_open_file(path.c_str(), 1, &ls_fp) != 0) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: lsmash_open_file failed: {}", path);
    return nullptr;
  }

  lsmash_root_t* ls_root = lsmash_create_root();
  lsmash_file_t* ls_file = lsmash_set_file(ls_root, &ls_fp);
  lsmash_read_file(ls_file, &ls_fp);

  lsmash_movie_parameters_t ls_mp{};
  if (lsmash_get_movie_parameters(ls_root, &ls_mp) != 0) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: lsmash_get_movie_parameters failed: {}", path);
    lsmash_destroy_root(ls_root);
    return nullptr;
  }

  // Iterate tracks: keep the first `meta` handler whose sample-entry
  // fourcc is `gpmd`. GoPro files typically have exactly one such
  // track.
  uint32_t gpmd_track_id = 0;
  uint32_t timescale     = 0;
  for (uint32_t track_n = 1; track_n <= ls_mp.number_of_tracks; ++track_n) {
    uint32_t const track_id = lsmash_get_track_ID(ls_root, track_n);

    lsmash_media_parameters_t ls_mdp{};
    MBASE_ASSERT(lsmash_get_media_parameters(ls_root, track_id, &ls_mdp) == 0);
    if (ls_mdp.handler_type != ISOM_MEDIA_HANDLER_TYPE_TIMED_METADATA_TRACK) continue;

    if (lsmash_construct_timeline(ls_root, track_id) != 0) {
      MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: lsmash_construct_timeline failed for meta track {}", track_id);
      continue;
    }

    bool is_gpmd = false;
    uint32_t const summary_count = lsmash_count_summary(ls_root, track_id);
    for (uint32_t i = 1; i <= summary_count; ++i) {
      lsmash_summary_t* s = lsmash_get_summary(ls_root, track_id, i);
      if (s != nullptr && s->sample_type.fourcc == kGpmdCodecFourcc) {
        is_gpmd = true;
        break;
      }
    }
    if (!is_gpmd) continue;

    gpmd_track_id = track_id;
    timescale     = ls_mdp.timescale;
    break;
  }
  if (gpmd_track_id == 0) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: no GPMF (meta/gpmd) track in {}", path);
    lsmash_destroy_root(ls_root);
    return nullptr;
  }

  uint32_t const total_sample_count = lsmash_get_sample_count_in_media_timeline(ls_root, gpmd_track_id);

  auto demuxer = std::unique_ptr<Mp4GpmfTrackDemuxer>(new Mp4GpmfTrackDemuxer{});
  demuxer->impl_->ls_root            = ls_root;
  demuxer->impl_->gpmd_track_id      = gpmd_track_id;
  demuxer->impl_->total_sample_count = total_sample_count;
  demuxer->impl_->timescale          = timescale;
  return demuxer;
}

Mp4GpmfTrackDemuxer::Sample Mp4GpmfTrackDemuxer::GetSampleByDecodeNo(uint32_t sample_no) {
  lsmash_sample_t* sample = lsmash_get_sample_from_media_timeline(impl_->ls_root, impl_->gpmd_track_id, sample_no);
  MBASE_ASSERT(sample != nullptr);

  Sample out;
  out.data.assign(sample->data, sample->data + sample->length);
  out.cts = sample->cts;

  // L-SMASH doesn't surface per-sample duration on `lsmash_sample_t`;
  // derive it from the next sample's CTS. The last sample has no
  // successor so we report duration = 0 -- the caller should treat
  // that as "active until end of track".
  if (sample_no < impl_->total_sample_count) {
    lsmash_sample_t* next = lsmash_get_sample_from_media_timeline(impl_->ls_root, impl_->gpmd_track_id, sample_no + 1);
    if (next != nullptr) {
      out.duration = static_cast<uint32_t>(next->cts - sample->cts);
      lsmash_delete_sample(next);
    }
  }

  lsmash_delete_sample(sample);
  return out;
}

uint32_t Mp4GpmfTrackDemuxer::total_sample_count() const { return impl_->total_sample_count; }
uint32_t Mp4GpmfTrackDemuxer::timescale()          const { return impl_->timescale; }

} // namespace mdemux
