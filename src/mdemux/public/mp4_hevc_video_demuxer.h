#pragma once

// c++ system headers -----------------------------------
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mdemux {

/// HDR static metadata extracted from the MP4 sample entry's `mdcv`
/// (Mastering Display Color Volume) and `clli` (Content Light Level
/// Info) boxes. All luminance values are in nits (cd/m^2) at the
/// natural integer precision; chromaticity coordinates are
/// 0.00002-step uint16 per ISOBMFF.
///
/// `*_present` is true when the corresponding box was found. HDR10
/// captures from phones / cameras typically carry both; SDR captures
/// carry neither.
struct Mp4HdrStaticMetadata final {
  bool     mdcv_present                       = false;
  uint16_t display_primaries_r_x              = 0;  // 0.00002 increments
  uint16_t display_primaries_r_y              = 0;
  uint16_t display_primaries_g_x              = 0;
  uint16_t display_primaries_g_y              = 0;
  uint16_t display_primaries_b_x              = 0;
  uint16_t display_primaries_b_y              = 0;
  uint16_t white_point_x                      = 0;
  uint16_t white_point_y                      = 0;
  /// Mastering display peak luminance, in nits (cd/m^2). Stored on
  /// disk in 0.0001-nit units; this accessor divides by 10000.
  uint32_t max_display_mastering_luminance    = 0;
  /// Min display luminance in raw 0.0001-nit units (typical: ~50 = 0.005 nits).
  uint32_t min_display_mastering_luminance_x10000 = 0;

  bool     clli_present                       = false;
  uint16_t max_content_light_level            = 0;  // MaxCLL in nits
  uint16_t max_pic_average_light_level        = 0;  // MaxFALL in nits
};

/// Color description copied from the MP4 sample entry's `colr` box.
/// All four fields use ITU-T H.273 / ISO 23001-8 indices when set,
/// with 2 = "unspecified" being the only well-known sentinel for
/// "not signalled". All-zero if no colr box is present in the sample
/// entry. The HEVC SPS VUI may carry the same information
/// independently; callers should reconcile.
struct Mp4ColorInfo final {
  uint16_t primaries_index = 0;   // 1 = BT.709, 9 = BT.2020
  uint16_t transfer_index  = 0;   // 1 = BT.709, 16 = SMPTE 2084 (HDR10), 18 = ARIB B67 (HLG)
  uint16_t matrix_index    = 0;   // 1 = BT.709, 9 = BT.2020 NCL
  uint8_t  full_range      = 0;
};

/// Demuxer for one HEVC video track of an ISOBMFF / MP4 file. Surfaces
/// the codec-specific data the decoder needs (raw `hvcC`), per-sample
/// bytes in decode order, and the display-order -> decode-order
/// permutation built from the (DTS, CTS) timestamps. HEVC parsing
/// itself happens elsewhere (see mhevcdec).
///
/// Multi-track files (e.g. GoPro Max 2 .360 carries two HEVC tracks)
/// are supported via the `video_track_index` parameter to `Open` --
/// 0-based across HEVC video tracks in declaration order. Pass
/// `EnumerateHevcVideoTracks(path)` to discover how many there are.
class Mp4HevcVideoDemuxer final {
public:
  /// One sample's bytes in length-prefixed (MP4) form, ready to be
  /// converted to Annex B or fed straight to a parser that knows
  /// `lengthSizeMinusOne`.
  struct Sample final {
    std::vector<uint8_t> data;
  };

  /// Opens `path`, locates the requested HEVC video track,
  /// constructs its timeline, and indexes IRAPs + the
  /// display/decode permutation. Returns `nullptr` on failure
  /// (file open, no matching track, no HEVC summary, etc.).
  static std::unique_ptr<Mp4HevcVideoDemuxer> Open(
    std::string const& path,
    uint32_t           video_track_index = 0);

  ~Mp4HevcVideoDemuxer();
  Mp4HevcVideoDemuxer(Mp4HevcVideoDemuxer const&) = delete;
  Mp4HevcVideoDemuxer& operator=(Mp4HevcVideoDemuxer const&) = delete;

  /// Raw bytes of the `hvcC` codec-specific data, ready to feed into
  /// an HEVC decoder configuration parser. Empty vector if absent
  /// (which would be a structural error for an HEVC track -- callers
  /// should assert).
  std::vector<uint8_t> GetHvcCBytes() const;

  /// Number of samples in the track's media timeline. Sample numbers
  /// are 1-indexed in decode order.
  uint32_t total_sample_count() const;

  /// Maps a 0-indexed display-order index into the 1-indexed
  /// decode-order sample number. Identity for IDR/P-only streams; a
  /// non-trivial permutation for B-frame streams.
  uint32_t DisplayToDecodeNo(uint32_t display_idx) const;

  /// Media timescale (ticks per second), straight from the track's
  /// media parameters.
  uint32_t timescale() const;

  /// CTS (composition time, in `timescale()` ticks) of the picture at
  /// the given 0-indexed display position. Sorted ascending across
  /// `display_idx`. Useful for wall-clock-driven playback pacing.
  uint64_t display_cts(uint32_t display_idx) const;

  /// `display_cts(display_idx)` divided by `timescale()`.
  double display_pts_seconds(uint32_t display_idx) const;

  /// Reads the bytes of `sample_no` (1-indexed, decode order). The
  /// returned `Sample` owns the bytes.
  Sample GetSampleByDecodeNo(uint32_t sample_no);

  /// Returns the largest IRAP (random-access point) decode-order
  /// sample number that is `<= decode_sample_no`. O(log N) lookup;
  /// the IRAP set is built once at `Open`. Asserts that sample 1 is
  /// itself an IRAP (true for any well-formed HEVC track).
  uint32_t PriorIrapSampleNo(uint32_t decode_sample_no) const;

  Mp4HdrStaticMetadata hdr_static_metadata() const;
  Mp4ColorInfo         color_info() const;

  /// Display rotation in degrees (one of 0 / 90 / 180 / 270),
  /// derived from the 3x3 transformation matrix in the track header
  /// (`tkhd`). Returns 0 if the matrix is identity OR not one of the
  /// four cardinal rotations (we don't model arbitrary affine
  /// transforms). For example a GoPro recorded upside-down reports
  /// 180.
  ///
  /// Note: this is only one of two possible sources -- HEVC streams
  /// can also carry a `display_orientation` SEI in the bitstream
  /// (typically one is identity if the other is set). Bitstream-side
  /// extraction is not done here.
  uint32_t rotation_degrees() const;

private:
  Mp4HevcVideoDemuxer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Returns the number of HEVC video tracks in `path`. Use this to
/// decide what `video_track_index` values are valid for
/// `Mp4HevcVideoDemuxer::Open`. Returns 0 on file-open failure.
uint32_t CountHevcVideoTracks(std::string const& path);

} // namespace mdemux
