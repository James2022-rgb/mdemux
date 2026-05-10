#pragma once

// c++ system headers -----------------------------------
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mdemux {

/// Demuxer for one AAC audio (MP4A) track of an ISOBMFF / MP4 file.
/// Sibling of `Mp4HevcVideoDemuxer`. Surfaces:
///  - the AAC AudioSpecificConfig (ASC) bytes from the `esds` box
///  - per-decode-order AAC sample bytes (raw frames, not ADTS)
///  - basic stream descriptors (sample rate, channel count, timescale,
///    sample count) for AV-sync and audio-sink configuration
///
/// Audio tracks have no display-vs-decode permutation, so there is no
/// `DisplayToDecodeNo` analogue.
class Mp4AacAudioDemuxer final {
public:
  /// One sample's raw AAC frame bytes (as stored in the MP4).
  struct Sample final {
    std::vector<uint8_t> data;
  };

  /// Opens `path`, locates the first AAC audio track, and constructs
  /// its timeline. Returns nullptr on failure (no audio track, no
  /// AAC summary, or L-SMASH error).
  static std::unique_ptr<Mp4AacAudioDemuxer> Open(std::string const& path);

  ~Mp4AacAudioDemuxer();
  Mp4AacAudioDemuxer(Mp4AacAudioDemuxer const&) = delete;
  Mp4AacAudioDemuxer& operator=(Mp4AacAudioDemuxer const&) = delete;

  /// Raw bytes of the AAC AudioSpecificConfig from the `esds` box.
  /// Empty vector if the track lacks one (which would be malformed
  /// for an mp4a audio track -- callers should assert).
  std::vector<uint8_t> GetAscBytes() const;

  uint32_t total_sample_count() const;
  uint32_t timescale()          const;
  uint32_t sample_rate()        const;
  uint32_t channel_count()      const;

  /// Reads sample `sample_no` (1-indexed, decode order) into an owned
  /// byte vector.
  Sample GetSampleByDecodeNo(uint32_t sample_no);

private:
  Mp4AacAudioDemuxer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mdemux
