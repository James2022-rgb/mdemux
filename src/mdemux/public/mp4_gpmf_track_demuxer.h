#pragma once

// c++ system headers -----------------------------------
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mdemux {

/// Demuxer for the GPMF (GoPro Metadata Format) timed-metadata track
/// of a GoPro MP4. Sibling of `Mp4HevcVideoDemuxer` /
/// `Mp4AacAudioDemuxer`. Surfaces per-sample raw GPMF byte chunks
/// (which the caller hands to a GPMF parser such as `mgpmf`) along
/// with the sample's composition time / duration so the caller can
/// align the telemetry to the video timeline.
///
/// Track selection: the first MP4 track whose handler type is
/// `meta` (ISOBMFF "timed metadata") AND whose sample-entry codec
/// fourcc is `gpmd`. GoPro files (HERO* / MAX*) carry exactly one
/// such track in normal usage. Multi-`gpmd`-track files are not
/// supported (would need a `gpmd_track_index` parameter -- add when
/// a real source shows up).
///
/// Timed metadata has no decode/display permutation and no random
/// access points -- every sample is independently parseable -- so
/// the API is intentionally smaller than the video demuxer's.
class Mp4GpmfTrackDemuxer final {
public:
  /// One `gpmd` MP4 sample: raw GPMF bytes plus the sample's
  /// timeline window. The bytes are the entire KLV stream for
  /// this sample (typically one second of GoPro telemetry at
  /// ~200 Hz ACCL / ~400 Hz GYRO / 10 Hz GPS9).
  struct Sample final {
    std::vector<uint8_t> data;
    /// Composition time in `timescale()` ticks. Sample N's
    /// telemetry window covers `[cts, cts + duration)`.
    uint64_t             cts      = 0;
    /// Sample duration in `timescale()` ticks.
    uint32_t             duration = 0;
  };

  /// Opens `path`, locates the first GPMF (`meta` handler, `gpmd`
  /// codec) track, and constructs its timeline. Returns `nullptr`
  /// on failure (file open, no GPMF track, L-SMASH error).
  static std::unique_ptr<Mp4GpmfTrackDemuxer> Open(std::string const& path);

  ~Mp4GpmfTrackDemuxer();
  Mp4GpmfTrackDemuxer(Mp4GpmfTrackDemuxer const&) = delete;
  Mp4GpmfTrackDemuxer& operator=(Mp4GpmfTrackDemuxer const&) = delete;

  /// Number of samples in the track's media timeline. Sample
  /// numbers are 1-indexed in decode order; decode order == display
  /// order for timed metadata.
  uint32_t total_sample_count() const;

  /// Media timescale (ticks per second), straight from the track's
  /// media parameters.
  uint32_t timescale() const;

  /// Reads sample `sample_no` (1-indexed) into an owned byte
  /// vector and copies its CTS / duration out of the L-SMASH
  /// sample struct.
  Sample GetSampleByDecodeNo(uint32_t sample_no);

private:
  Mp4GpmfTrackDemuxer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mdemux
