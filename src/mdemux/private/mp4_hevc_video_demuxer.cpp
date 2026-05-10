// TU header --------------------------------------------
#include "mdemux/public/mp4_hevc_video_demuxer.h"

// c++ system headers -----------------------------------
#include <algorithm>
#include <cstring>
#include <utility>

// public project headers -------------------------------
#include "mbase/public/assert.h"
#include "mbase/public/log.h"

// external headers -------------------------------------
#include "lsmash.h"

namespace mdemux {

struct Mp4HevcVideoDemuxer::Impl {
  lsmash_root_t*    ls_root            = nullptr;
  uint32_t          video_track_id     = 0;
  lsmash_summary_t* ls_summary         = nullptr;  // borrowed from ls_root
  uint32_t          total_sample_count = 0;
  uint32_t          timescale          = 0;
  uint32_t          rotation_degrees   = 0;     // 0 / 90 / 180 / 270
  Mp4ColorInfo      color_info{};
  Mp4HdrStaticMetadata hdr_static_metadata{};
  std::vector<uint32_t> display_order_to_decode_sample_no;
  std::vector<uint64_t> display_order_cts;
  std::vector<uint32_t> irap_decode_nos_sorted;

  ~Impl() {
    if (ls_root != nullptr) {
      lsmash_destroy_root(ls_root);
      ls_root = nullptr;
    }
  }
};

Mp4HevcVideoDemuxer::Mp4HevcVideoDemuxer() : impl_(std::make_unique<Impl>()) {}
Mp4HevcVideoDemuxer::~Mp4HevcVideoDemuxer() = default;

namespace {

/// Walks the track table of an already-opened L-SMASH root and finds
/// the `video_track_index`-th HEVC video track (0-based across HEVC
/// tracks, in declaration order). Returns 0 on miss.
uint32_t FindNthHevcVideoTrack(lsmash_root_t* ls_root, uint32_t video_track_index, uint32_t& out_timescale) {
  lsmash_movie_parameters_t ls_mp{};
  if (lsmash_get_movie_parameters(ls_root, &ls_mp) != 0) {
    return 0;
  }
  uint32_t hevc_seen = 0;
  for (uint32_t track_n = 1; track_n <= ls_mp.number_of_tracks; ++track_n) {
    uint32_t const track_id = lsmash_get_track_ID(ls_root, track_n);
    lsmash_media_parameters_t ls_mdp{};
    if (lsmash_get_media_parameters(ls_root, track_id, &ls_mdp) != 0) continue;
    if (ls_mdp.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK) continue;

    // Probe the sample-entry summaries for HEVC. A track is HEVC iff
    // any summary fourcc is hvc1 / hev1.
    bool is_hevc = false;
    uint32_t const summary_count = lsmash_count_summary(ls_root, track_id);
    for (uint32_t i = 1; i <= summary_count; ++i) {
      lsmash_summary_t* s = lsmash_get_summary(ls_root, track_id, i);
      if (s == nullptr) continue;
      if (s->sample_type.fourcc == ISOM_CODEC_TYPE_HVC1_VIDEO.fourcc ||
          s->sample_type.fourcc == ISOM_CODEC_TYPE_HEV1_VIDEO.fourcc) {
        is_hevc = true;
        break;
      }
    }
    if (!is_hevc) continue;

    if (hevc_seen == video_track_index) {
      out_timescale = ls_mdp.timescale;
      return track_id;
    }
    ++hevc_seen;
  }
  return 0;
}

} // namespace

uint32_t CountHevcVideoTracks(std::string const& path) {
  lsmash_file_parameters_t ls_fp{};
  if (lsmash_open_file(path.c_str(), 1, &ls_fp) != 0) return 0;
  lsmash_root_t* ls_root = lsmash_create_root();
  lsmash_file_t* ls_file = lsmash_set_file(ls_root, &ls_fp);
  lsmash_read_file(ls_file, &ls_fp);

  lsmash_movie_parameters_t ls_mp{};
  if (lsmash_get_movie_parameters(ls_root, &ls_mp) != 0) {
    lsmash_destroy_root(ls_root);
    return 0;
  }
  uint32_t hevc_count = 0;
  for (uint32_t track_n = 1; track_n <= ls_mp.number_of_tracks; ++track_n) {
    uint32_t const track_id = lsmash_get_track_ID(ls_root, track_n);
    lsmash_media_parameters_t ls_mdp{};
    if (lsmash_get_media_parameters(ls_root, track_id, &ls_mdp) != 0) continue;
    if (ls_mdp.handler_type != ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK) continue;
    uint32_t const summary_count = lsmash_count_summary(ls_root, track_id);
    for (uint32_t i = 1; i <= summary_count; ++i) {
      lsmash_summary_t* s = lsmash_get_summary(ls_root, track_id, i);
      if (s == nullptr) continue;
      if (s->sample_type.fourcc == ISOM_CODEC_TYPE_HVC1_VIDEO.fourcc ||
          s->sample_type.fourcc == ISOM_CODEC_TYPE_HEV1_VIDEO.fourcc) {
        ++hevc_count;
        break;
      }
    }
  }
  lsmash_destroy_root(ls_root);
  return hevc_count;
}

std::unique_ptr<Mp4HevcVideoDemuxer> Mp4HevcVideoDemuxer::Open(
  std::string const& path,
  uint32_t           video_track_index)
{
  lsmash_file_parameters_t ls_fp{};
  if (lsmash_open_file(path.c_str(), 1, &ls_fp) != 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoDemuxer: lsmash_open_file failed: {}", path);
    return nullptr;
  }

  lsmash_root_t* ls_root = lsmash_create_root();
  lsmash_file_t* ls_file = lsmash_set_file(ls_root, &ls_fp);
  lsmash_read_file(ls_file, &ls_fp);

  uint32_t timescale = 0;
  uint32_t const video_track_id = FindNthHevcVideoTrack(ls_root, video_track_index, timescale);
  if (video_track_id == 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoDemuxer: HEVC video track index {} not found in {}", video_track_index, path);
    lsmash_destroy_root(ls_root);
    return nullptr;
  }

  // Read the tkhd transformation matrix and convert it to a cardinal
  // rotation (0 / 90 / 180 / 270). Anything else (skew, non-rigid
  // transforms, identity) collapses to 0 -- we only need axis-aligned
  // image rotation today.
  uint32_t rotation_degrees = 0;
  {
    lsmash_track_parameters_t tp{};
    lsmash_initialize_track_parameters(&tp);
    if (lsmash_get_track_parameters(ls_root, video_track_id, &tp) == 0) {
      // 16.16 fixed-point. Matrix layout: [a b u; c d v; x y w].
      int32_t const a = tp.matrix[0];
      int32_t const b = tp.matrix[1];
      int32_t const c = tp.matrix[3];
      int32_t const d = tp.matrix[4];
      constexpr int32_t kOne  = 0x10000;
      constexpr int32_t kZero = 0;
      if      (a ==  kOne && b == kZero && c == kZero && d ==  kOne) rotation_degrees =   0;
      else if (a == kZero && b ==  kOne && c == -kOne && d == kZero) rotation_degrees =  90;
      else if (a == -kOne && b == kZero && c == kZero && d == -kOne) rotation_degrees = 180;
      else if (a == kZero && b == -kOne && c ==  kOne && d == kZero) rotation_degrees = 270;
      MBASE_LOG_INFO(
        "Mp4HevcVideoDemuxer: tkhd matrix=[a={:#x} b={:#x} c={:#x} d={:#x}] -> rotation={} deg",
        static_cast<uint32_t>(a), static_cast<uint32_t>(b),
        static_cast<uint32_t>(c), static_cast<uint32_t>(d), rotation_degrees);
    }
  }

  if (lsmash_construct_timeline(ls_root, video_track_id) != 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoDemuxer: lsmash_construct_timeline failed for track {}", video_track_id);
    lsmash_destroy_root(ls_root);
    return nullptr;
  }

  // Locate the HEVC sample-entry summary on this track.
  lsmash_summary_t* ls_summary = nullptr;
  uint32_t const summary_count = lsmash_count_summary(ls_root, video_track_id);
  for (uint32_t i = 1; i <= summary_count; ++i) {
    lsmash_summary_t* s = lsmash_get_summary(ls_root, video_track_id, i);
    if (s->sample_type.fourcc == ISOM_CODEC_TYPE_HVC1_VIDEO.fourcc ||
        s->sample_type.fourcc == ISOM_CODEC_TYPE_HEV1_VIDEO.fourcc) {
      ls_summary = s;
      break;
    }
  }
  if (ls_summary == nullptr) {
    MBASE_LOG_ERROR("Mp4HevcVideoDemuxer: no HEVC summary found in track {}", video_track_id);
    lsmash_destroy_root(ls_root);
    return nullptr;
  }

  // Pull the `colr` box's color description (primaries / transfer /
  // matrix / full_range) straight off the video summary.
  Mp4ColorInfo color_info{};
  {
    auto const* vs = reinterpret_cast<lsmash_video_summary_t const*>(ls_summary);
    color_info.primaries_index = vs->color.primaries_index;
    color_info.transfer_index  = vs->color.transfer_index;
    color_info.matrix_index    = vs->color.matrix_index;
    color_info.full_range      = vs->color.full_range;
  }

  // Pull `mdcv` (Mastering Display Color Volume) and `clli` (Content
  // Light Level Info) from the sample entry's codec-specific data
  // list. Both are stored on the QT-named codec specific types but
  // the parser writes the same fields for ISOBMFF inputs. Either or
  // both may be absent (typical for SDR), in which case the
  // respective `*_present` stays false.
  Mp4HdrStaticMetadata hdr_meta{};
  {
    uint32_t const cs_count = lsmash_count_codec_specific_data(ls_summary);
    for (uint32_t i = 1; i <= cs_count; ++i) {
      lsmash_codec_specific_t* const spec_in = lsmash_get_codec_specific_data(ls_summary, i);
      if (spec_in == nullptr) continue;

      if (spec_in->type == LSMASH_CODEC_SPECIFIC_DATA_TYPE_QT_VIDEO_MASTERING_DISPLAY_COLOR_VOLUME) {
        lsmash_codec_specific_t* const spec =
          lsmash_convert_codec_specific_format(spec_in, LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED);
        if (spec != nullptr && spec->data.structured != nullptr) {
          auto const* m = static_cast<lsmash_qt_mastering_display_color_volume_t const*>(spec->data.structured);
          hdr_meta.mdcv_present                 = true;
          hdr_meta.display_primaries_r_x        = m->display_primaries_r_x;
          hdr_meta.display_primaries_r_y        = m->display_primaries_r_y;
          hdr_meta.display_primaries_g_x        = m->display_primaries_g_x;
          hdr_meta.display_primaries_g_y        = m->display_primaries_g_y;
          hdr_meta.display_primaries_b_x        = m->display_primaries_b_x;
          hdr_meta.display_primaries_b_y        = m->display_primaries_b_y;
          hdr_meta.white_point_x                = m->white_point_x;
          hdr_meta.white_point_y                = m->white_point_y;
          // On disk: 0.0001 nit units (per ISO 23001-8). Drop the
          // fractional part for the public accessor; keep the raw
          // 0.0001-unit min as-is (typical values are < 1 nit).
          hdr_meta.max_display_mastering_luminance        = m->max_display_mastering_luminance / 10000u;
          hdr_meta.min_display_mastering_luminance_x10000 = m->min_display_mastering_luminance;
          lsmash_destroy_codec_specific_data(spec);
        }
      } else if (spec_in->type == LSMASH_CODEC_SPECIFIC_DATA_TYPE_QT_VIDEO_CONTENT_LIGHT_LEVEL_INFO) {
        lsmash_codec_specific_t* const spec =
          lsmash_convert_codec_specific_format(spec_in, LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED);
        if (spec != nullptr && spec->data.structured != nullptr) {
          auto const* c = static_cast<lsmash_qt_content_light_level_info_t const*>(spec->data.structured);
          hdr_meta.clli_present                = true;
          hdr_meta.max_content_light_level     = c->max_content_light_level;
          hdr_meta.max_pic_average_light_level = c->max_pic_average_light_level;
          lsmash_destroy_codec_specific_data(spec);
        }
      }
    }
    if (hdr_meta.mdcv_present || hdr_meta.clli_present) {
      MBASE_LOG_INFO("Mp4HevcVideoDemuxer: HDR static metadata mdcv={} clli={} max_disp_lum={} nits MaxCLL={} MaxFALL={}",
        hdr_meta.mdcv_present ? "yes" : "no",
        hdr_meta.clli_present ? "yes" : "no",
        hdr_meta.max_display_mastering_luminance,
        hdr_meta.max_content_light_level,
        hdr_meta.max_pic_average_light_level);
    }
  }

  uint32_t const total_sample_count = lsmash_get_sample_count_in_media_timeline(ls_root, video_track_id);

  // Pre-index every IRAP (random-access point) in decode order so
  // backward seeks can jump to the prior IRAP in O(log N) instead of
  // re-fetching and re-parsing samples one at a time. L-SMASH's
  // `ra_flags` already carries this from the `stss` (sync sample) box
  // and any sample group descriptions in the moov, so this loop just
  // walks the in-memory timeline and never touches the file.
  std::vector<uint32_t> irap_decode_nos_sorted;
  {
    irap_decode_nos_sorted.reserve(total_sample_count / 30u + 8u);
    constexpr uint8_t kRapMask =
      ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC | ISOM_SAMPLE_RANDOM_ACCESS_FLAG_RAP;
    for (uint32_t no = 1; no <= total_sample_count; ++no) {
      lsmash_sample_property_t prop{};
      if (lsmash_get_sample_property_from_media_timeline(ls_root, video_track_id, no, &prop) != 0) {
        continue;
      }
      if ((prop.ra_flags & kRapMask) != 0) {
        irap_decode_nos_sorted.push_back(no);
      }
    }
    MBASE_ASSERT_MSG(!irap_decode_nos_sorted.empty() && irap_decode_nos_sorted.front() == 1,
      "Mp4HevcVideoDemuxer: no IRAP at sample 1 -- track is not random-accessible from the start.");
    MBASE_LOG_INFO("Mp4HevcVideoDemuxer: indexed {} IRAPs in {} samples",
      irap_decode_nos_sorted.size(), total_sample_count);
  }

  // Build the display-order -> decode-order permutation. L-SMASH numbers
  // samples 1..N in decode order; sorting by CTS gives display order.
  // We also retain the per-display-index CTS for wall-clock-driven
  // playback pacing.
  std::vector<uint32_t> display_order_to_decode_sample_no;
  std::vector<uint64_t> display_order_cts;
  {
    lsmash_media_ts_list_t ts_list{};
    if (lsmash_get_media_timestamps(ls_root, video_track_id, &ts_list) != 0) {
      MBASE_LOG_ERROR("Mp4HevcVideoDemuxer: lsmash_get_media_timestamps failed");
      lsmash_destroy_root(ls_root);
      return nullptr;
    }
    MBASE_ASSERT(ts_list.sample_count == total_sample_count);

    std::vector<std::pair<uint64_t, uint32_t>> cts_to_decode_no;
    cts_to_decode_no.reserve(ts_list.sample_count);
    for (uint32_t i = 0; i < ts_list.sample_count; ++i) {
      uint32_t const decode_sample_no = i + 1;
      cts_to_decode_no.emplace_back(ts_list.timestamp[i].cts, decode_sample_no);
    }
    lsmash_delete_media_timestamps(&ts_list);

    std::sort(cts_to_decode_no.begin(), cts_to_decode_no.end(),
              [](auto const& a, auto const& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;  // tie-break by decode_no
              });

    display_order_to_decode_sample_no.resize(total_sample_count);
    display_order_cts.resize(total_sample_count);
    for (uint32_t i = 0; i < total_sample_count; ++i) {
      display_order_to_decode_sample_no[i] = cts_to_decode_no[i].second;
      display_order_cts[i]                 = cts_to_decode_no[i].first;
    }
  }

  auto demuxer = std::unique_ptr<Mp4HevcVideoDemuxer>(new Mp4HevcVideoDemuxer{});
  demuxer->impl_->ls_root                          = ls_root;
  demuxer->impl_->video_track_id                   = video_track_id;
  demuxer->impl_->ls_summary                       = ls_summary;
  demuxer->impl_->total_sample_count               = total_sample_count;
  demuxer->impl_->timescale                        = timescale;
  demuxer->impl_->rotation_degrees                 = rotation_degrees;
  demuxer->impl_->color_info                       = color_info;
  demuxer->impl_->hdr_static_metadata              = hdr_meta;
  demuxer->impl_->display_order_to_decode_sample_no = std::move(display_order_to_decode_sample_no);
  demuxer->impl_->display_order_cts                = std::move(display_order_cts);
  demuxer->impl_->irap_decode_nos_sorted           = std::move(irap_decode_nos_sorted);
  return demuxer;
}

uint32_t Mp4HevcVideoDemuxer::PriorIrapSampleNo(uint32_t decode_sample_no) const {
  MBASE_ASSERT(!impl_->irap_decode_nos_sorted.empty());
  // upper_bound returns the first element strictly greater than the
  // target; the element immediately before it is the largest IRAP <=
  // target. Sample 1 is always an IRAP (asserted at Open), so the
  // returned iterator is never `begin()`.
  auto const it = std::upper_bound(
    impl_->irap_decode_nos_sorted.begin(), impl_->irap_decode_nos_sorted.end(), decode_sample_no);
  MBASE_ASSERT(it != impl_->irap_decode_nos_sorted.begin());
  return *(it - 1);
}

std::vector<uint8_t> Mp4HevcVideoDemuxer::GetHvcCBytes() const {
  MBASE_ASSERT(impl_->ls_summary != nullptr);
  uint32_t const cs_count = lsmash_count_codec_specific_data(impl_->ls_summary);
  for (uint32_t i = 1; i <= cs_count; ++i) {
    lsmash_codec_specific_t* spec = lsmash_get_codec_specific_data(impl_->ls_summary, i);
    if (spec->type != LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_HEVC) continue;

    lsmash_codec_specific_t* raw =
      lsmash_convert_codec_specific_format(spec, LSMASH_CODEC_SPECIFIC_FORMAT_UNSTRUCTURED);
    std::vector<uint8_t> out(raw->size);
    std::memcpy(out.data(), raw->data.unstructured, raw->size);
    return out;
  }
  return {};
}

Mp4HevcVideoDemuxer::Sample Mp4HevcVideoDemuxer::GetSampleByDecodeNo(uint32_t sample_no) {
  lsmash_sample_t* sample = lsmash_get_sample_from_media_timeline(impl_->ls_root, impl_->video_track_id, sample_no);
  MBASE_ASSERT(sample != nullptr);
  Sample out;
  out.data.assign(sample->data, sample->data + sample->length);
  lsmash_delete_sample(sample);
  return out;
}

uint32_t Mp4HevcVideoDemuxer::total_sample_count()       const { return impl_->total_sample_count; }
uint32_t Mp4HevcVideoDemuxer::timescale()                const { return impl_->timescale; }
uint32_t Mp4HevcVideoDemuxer::DisplayToDecodeNo(uint32_t display_idx) const {
  return impl_->display_order_to_decode_sample_no[display_idx];
}
uint64_t Mp4HevcVideoDemuxer::display_cts(uint32_t display_idx) const {
  return impl_->display_order_cts[display_idx];
}
double   Mp4HevcVideoDemuxer::display_pts_seconds(uint32_t display_idx) const {
  return static_cast<double>(display_cts(display_idx)) / static_cast<double>(impl_->timescale);
}
Mp4HdrStaticMetadata Mp4HevcVideoDemuxer::hdr_static_metadata() const { return impl_->hdr_static_metadata; }
Mp4ColorInfo         Mp4HevcVideoDemuxer::color_info()          const { return impl_->color_info; }
uint32_t             Mp4HevcVideoDemuxer::rotation_degrees()    const { return impl_->rotation_degrees; }

} // namespace mdemux
