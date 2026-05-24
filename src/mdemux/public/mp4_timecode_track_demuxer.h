#pragma once

// c++ system headers -----------------------------------
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mdemux {

/// Demuxer for the QuickTime `tmcd` timecode track of an MP4 (the
/// kind GoPro / phone cameras attach to record wall-clock capture
/// time). Sibling of `Mp4HevcVideoDemuxer` / `Mp4AacAudioDemuxer` /
/// `Mp4GpmfTrackDemuxer`. Surfaces:
///   - the per-sample raw 32-bit timecode value bytes
///   - the tmcd SampleEntry metadata (timescale / frame_duration /
///     fps_int / flags) needed to re-write the track into a new MP4
///
/// tmcd tracks typically carry one sample for the whole movie -- the
/// start-of-recording timecode -- but the API supports multi-sample
/// tracks too.
class Mp4TimecodeTrackDemuxer final {
public:
  /// One `tmcd` sample (almost always 4 bytes big-endian uint32).
  struct Sample final {
    std::vector<uint8_t> data;
    uint64_t             cts      = 0;
    uint32_t             duration = 0;
  };

  /// Opens `path`, locates the first track with handler type `tmcd`,
  /// and constructs its timeline. Returns `nullptr` on failure
  /// (file open, no tmcd track, L-SMASH error).
  static std::unique_ptr<Mp4TimecodeTrackDemuxer> Open(std::string const& path);

  ~Mp4TimecodeTrackDemuxer();
  Mp4TimecodeTrackDemuxer(Mp4TimecodeTrackDemuxer const&) = delete;
  Mp4TimecodeTrackDemuxer& operator=(Mp4TimecodeTrackDemuxer const&) = delete;

  uint32_t total_sample_count() const;
  uint32_t timescale()          const;

  /// `frame_duration` in `timescale` ticks per frame. fps =
  /// `timescale / frame_duration`.
  uint32_t frame_duration() const;
  /// Number of timecode frames per integer second (e.g. 30 for
  /// 30000/1001 fps).
  uint8_t  fps_int() const;
  /// QuickTime tmcd flags (bit 0 drop-frame, etc).
  uint32_t flags() const;

  Sample GetSampleByDecodeNo(uint32_t sample_no);

private:
  Mp4TimecodeTrackDemuxer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mdemux
