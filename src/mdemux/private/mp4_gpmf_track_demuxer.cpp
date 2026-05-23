// TU header --------------------------------------------
#include "mdemux/public/mp4_gpmf_track_demuxer.h"

// c++ system headers -----------------------------------
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

// public project headers -------------------------------
#include "mbase/public/assert.h"
#include "mbase/public/log.h"

// ISOBMFF box walker built from scratch because L-SMASH treats the
// `gpmd` sample entry as an unknown box (stored in extensions, not
// stsd->list), which makes `lsmash_construct_timeline` reject the
// track. Walking the box hierarchy ourselves lets us reach the same
// sample-table information (stco / stsz / stsc / stts) that L-SMASH
// would have surfaced for a "known" codec.

namespace mdemux {

namespace {

// All multi-byte ISOBMFF integers are big-endian.
inline uint16_t ReadBE16(uint8_t const* p) {
  return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
}
inline uint32_t ReadBE32(uint8_t const* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
       | (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
}
inline uint64_t ReadBE64(uint8_t const* p) {
  return (uint64_t(ReadBE32(p)) << 32) | ReadBE32(p + 4);
}

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24)
       | (uint32_t(uint8_t(b)) << 16)
       | (uint32_t(uint8_t(c)) <<  8)
       |  uint32_t(uint8_t(d));
}

/// Portable 64-bit fseek. Windows MSVC's `long` is 32-bit, so plain
/// `fseek` would overflow on >2 GB offsets even though MP4 `co64`
/// boxes can carry full 64-bit offsets. Returns 0 on success.
inline int Seek64(std::FILE* fp, int64_t offset, int whence) {
#if defined(_MSC_VER)
  return _fseeki64(fp, offset, whence);
#else
  return std::fseek(fp, static_cast<long>(offset), whence);
#endif
}

constexpr uint32_t kFourcc_moov = Fourcc('m','o','o','v');
constexpr uint32_t kFourcc_trak = Fourcc('t','r','a','k');
constexpr uint32_t kFourcc_mdia = Fourcc('m','d','i','a');
constexpr uint32_t kFourcc_mdhd = Fourcc('m','d','h','d');
constexpr uint32_t kFourcc_hdlr = Fourcc('h','d','l','r');
constexpr uint32_t kFourcc_minf = Fourcc('m','i','n','f');
constexpr uint32_t kFourcc_stbl = Fourcc('s','t','b','l');
constexpr uint32_t kFourcc_stsd = Fourcc('s','t','s','d');
constexpr uint32_t kFourcc_stts = Fourcc('s','t','t','s');
constexpr uint32_t kFourcc_stsc = Fourcc('s','t','s','c');
constexpr uint32_t kFourcc_stsz = Fourcc('s','t','s','z');
constexpr uint32_t kFourcc_stz2 = Fourcc('s','t','z','2');
constexpr uint32_t kFourcc_stco = Fourcc('s','t','c','o');
constexpr uint32_t kFourcc_co64 = Fourcc('c','o','6','4');
constexpr uint32_t kFourcc_meta = Fourcc('m','e','t','a');
constexpr uint32_t kFourcc_gpmd = Fourcc('g','p','m','d');

/// One child box inside a container's payload.
struct Box final {
  uint32_t       fourcc       = 0;
  uint8_t const* payload      = nullptr;
  size_t         payload_size = 0;
};

/// Iterate child boxes within `[data, data+len)`. The callback
/// returns false to stop iteration early.
template <typename F>
void ForEachChild(uint8_t const* data, size_t len, F&& callback) {
  size_t pos = 0;
  while (pos + 8 <= len) {
    uint32_t const size = ReadBE32(data + pos);
    uint32_t const fourcc = ReadBE32(data + pos + 4);
    size_t   header_size = 8;
    uint64_t total_size  = size;
    if (size == 1u) {
      if (pos + 16u > len) break;
      total_size = ReadBE64(data + pos + 8);
      header_size = 16u;
    } else if (size == 0u) {
      total_size = len - pos; // extends to end of parent
    }
    if (total_size < header_size || pos + total_size > len) break;

    Box const child{
      .fourcc       = fourcc,
      .payload      = data + pos + header_size,
      .payload_size = static_cast<size_t>(total_size - header_size),
    };
    callback(child);
    pos += static_cast<size_t>(total_size);
  }
}

/// Read one box's header from `fp` and return its fourcc, total
/// (= header + payload) size, and header byte count. Returns false
/// at EOF / short read.
struct TopBoxHeader final { uint32_t fourcc; uint64_t total_size; uint32_t header_size; };
bool ReadTopBoxHeader(std::FILE* fp, TopBoxHeader& out) {
  uint8_t hdr[16];
  if (std::fread(hdr, 1, 8, fp) != 8) return false;
  uint32_t const size = ReadBE32(hdr);
  out.fourcc = ReadBE32(hdr + 4);
  if (size == 1u) {
    if (std::fread(hdr + 8, 1, 8, fp) != 8) return false;
    out.total_size  = ReadBE64(hdr + 8);
    out.header_size = 16u;
  } else if (size == 0u) {
    // To EOF -- we don't expect this for moov; treat as failure.
    return false;
  } else {
    out.total_size  = size;
    out.header_size = 8u;
  }
  return out.total_size >= out.header_size;
}

/// Look up the entry in `stsc_list` that governs the given (1-indexed)
/// chunk. The stsc table is run-length: each entry's `first_chunk`
/// marks the start of a run, and `samples_per_chunk` applies until
/// the next entry's `first_chunk`.
uint32_t SamplesInChunk(
  std::vector<std::tuple<uint32_t /*first_chunk*/,
                         uint32_t /*samples_per_chunk*/>> const& stsc_list,
  uint32_t chunk_no)
{
  uint32_t samples_per_chunk = 0;
  for (auto const& entry : stsc_list) {
    if (std::get<0>(entry) > chunk_no) break;
    samples_per_chunk = std::get<1>(entry);
  }
  return samples_per_chunk;
}

} // namespace

struct Mp4GpmfTrackDemuxer::Impl {
  std::FILE* fp                 = nullptr;
  uint32_t   total_sample_count = 0;
  uint32_t   timescale          = 0;

  struct SampleEntry final {
    uint64_t file_offset = 0;
    uint32_t length      = 0;
    uint64_t cts         = 0;
    uint32_t duration    = 0;
  };
  std::vector<SampleEntry> samples;

  ~Impl() {
    if (fp != nullptr) {
      std::fclose(fp);
      fp = nullptr;
    }
  }
};

Mp4GpmfTrackDemuxer::Mp4GpmfTrackDemuxer() : impl_(std::make_unique<Impl>()) {}
Mp4GpmfTrackDemuxer::~Mp4GpmfTrackDemuxer() = default;

std::unique_ptr<Mp4GpmfTrackDemuxer> Mp4GpmfTrackDemuxer::Open(std::string const& path) {
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: fopen failed: {}", path);
    return nullptr;
  }

  // ---- Locate and slurp the moov box. -------------------------
  std::vector<uint8_t> moov_payload;
  {
    TopBoxHeader bh{};
    while (ReadTopBoxHeader(fp, bh)) {
      uint64_t const payload_size = bh.total_size - bh.header_size;
      if (bh.fourcc == kFourcc_moov) {
        moov_payload.resize(static_cast<size_t>(payload_size));
        if (std::fread(moov_payload.data(), 1, moov_payload.size(), fp) != moov_payload.size()) {
          MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: short read on moov in {}", path);
          std::fclose(fp);
          return nullptr;
        }
        break;
      }
      // Skip this top-level box's payload.
      if (Seek64(fp, static_cast<int64_t>(payload_size), SEEK_CUR) != 0) {
        std::fclose(fp);
        return nullptr;
      }
    }
  }
  if (moov_payload.empty()) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: no moov box in {}", path);
    std::fclose(fp);
    return nullptr;
  }

  // ---- For each trak, test whether it's the gpmd track. -------
  bool                                                  gpmd_found = false;
  uint32_t                                              timescale  = 0;
  Box                                                   stbl_box{};  // captured for the gpmd track
  ForEachChild(moov_payload.data(), moov_payload.size(), [&](Box const& moov_child) {
    if (gpmd_found || moov_child.fourcc != kFourcc_trak) return;

    Box mdia_box{};
    ForEachChild(moov_child.payload, moov_child.payload_size, [&](Box const& c) {
      if (c.fourcc == kFourcc_mdia) mdia_box = c;
    });
    if (mdia_box.payload == nullptr) return;

    uint32_t handler_type = 0;
    uint32_t track_timescale = 0;
    Box minf_box{};
    ForEachChild(mdia_box.payload, mdia_box.payload_size, [&](Box const& c) {
      if (c.fourcc == kFourcc_mdhd && c.payload_size >= 20) {
        // version(1) + flags(3) + creation(4|8) + modif(4|8) + timescale(4) + ...
        uint8_t const version = c.payload[0];
        size_t const off = (version == 1) ? (4 + 8 + 8) : (4 + 4 + 4);
        if (c.payload_size >= off + 4) {
          track_timescale = ReadBE32(c.payload + off);
        }
      } else if (c.fourcc == kFourcc_hdlr && c.payload_size >= 12) {
        // version(1) + flags(3) + pre_defined(4) + handler_type(4) + ...
        handler_type = ReadBE32(c.payload + 8);
      } else if (c.fourcc == kFourcc_minf) {
        minf_box = c;
      }
    });
    if (handler_type != kFourcc_meta || minf_box.payload == nullptr) return;

    // Walk minf -> stbl, find stsd's first sample entry, check its fourcc.
    Box this_stbl_box{};
    ForEachChild(minf_box.payload, minf_box.payload_size, [&](Box const& c) {
      if (c.fourcc == kFourcc_stbl) this_stbl_box = c;
    });
    if (this_stbl_box.payload == nullptr) return;

    bool stsd_is_gpmd = false;
    ForEachChild(this_stbl_box.payload, this_stbl_box.payload_size, [&](Box const& c) {
      if (c.fourcc != kFourcc_stsd) return;
      // stsd: version(1) + flags(3) + entry_count(4) + first sample entry box
      if (c.payload_size < 8 + 8) return;
      uint32_t const entry_count = ReadBE32(c.payload + 4);
      if (entry_count == 0) return;
      // First entry box starts at payload+8: size(4) + type(4) + ...
      uint32_t const first_entry_type = ReadBE32(c.payload + 8 + 4);
      stsd_is_gpmd = (first_entry_type == kFourcc_gpmd);
    });
    if (!stsd_is_gpmd) return;

    gpmd_found = true;
    timescale  = track_timescale;
    stbl_box   = this_stbl_box;
  });

  if (!gpmd_found) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: no GPMF (meta/gpmd) track in {}", path);
    std::fclose(fp);
    return nullptr;
  }
  if (timescale == 0) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: gpmd track has zero timescale in {}", path);
    std::fclose(fp);
    return nullptr;
  }

  // ---- Parse the stbl children into sample tables. ------------
  std::vector<std::tuple<uint32_t, uint32_t>> stsc_list; // (first_chunk, samples_per_chunk)
  std::vector<uint64_t>                       chunk_offsets;
  std::vector<uint32_t>                       sample_sizes;
  uint32_t                                    constant_sample_size = 0; // when stsz.sample_size != 0
  uint32_t                                    sample_count_in_stsz = 0;
  std::vector<std::tuple<uint32_t, uint32_t>> stts_list; // (sample_count, sample_delta)

  ForEachChild(stbl_box.payload, stbl_box.payload_size, [&](Box const& c) {
    if (c.fourcc == kFourcc_stsc && c.payload_size >= 8) {
      uint32_t const ec = ReadBE32(c.payload + 4);
      stsc_list.reserve(ec);
      size_t off = 8;
      for (uint32_t i = 0; i < ec && off + 12 <= c.payload_size; ++i, off += 12) {
        uint32_t const first_chunk        = ReadBE32(c.payload + off);
        uint32_t const samples_per_chunk  = ReadBE32(c.payload + off + 4);
        // (sample_description_index at off + 8 -- ignored: we assume gpmd)
        stsc_list.emplace_back(first_chunk, samples_per_chunk);
      }
    } else if (c.fourcc == kFourcc_stco && c.payload_size >= 8) {
      uint32_t const ec = ReadBE32(c.payload + 4);
      chunk_offsets.reserve(ec);
      size_t off = 8;
      for (uint32_t i = 0; i < ec && off + 4 <= c.payload_size; ++i, off += 4) {
        chunk_offsets.push_back(ReadBE32(c.payload + off));
      }
    } else if (c.fourcc == kFourcc_co64 && c.payload_size >= 8) {
      uint32_t const ec = ReadBE32(c.payload + 4);
      chunk_offsets.reserve(ec);
      size_t off = 8;
      for (uint32_t i = 0; i < ec && off + 8 <= c.payload_size; ++i, off += 8) {
        chunk_offsets.push_back(ReadBE64(c.payload + off));
      }
    } else if (c.fourcc == kFourcc_stsz && c.payload_size >= 12) {
      constant_sample_size = ReadBE32(c.payload + 4);
      sample_count_in_stsz = ReadBE32(c.payload + 8);
      if (constant_sample_size == 0) {
        sample_sizes.reserve(sample_count_in_stsz);
        size_t off = 12;
        for (uint32_t i = 0; i < sample_count_in_stsz && off + 4 <= c.payload_size; ++i, off += 4) {
          sample_sizes.push_back(ReadBE32(c.payload + off));
        }
      }
    } else if (c.fourcc == kFourcc_stz2) {
      MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: stz2 (variable field size) not supported");
    } else if (c.fourcc == kFourcc_stts && c.payload_size >= 8) {
      uint32_t const ec = ReadBE32(c.payload + 4);
      stts_list.reserve(ec);
      size_t off = 8;
      for (uint32_t i = 0; i < ec && off + 8 <= c.payload_size; ++i, off += 8) {
        uint32_t const sc = ReadBE32(c.payload + off);
        uint32_t const sd = ReadBE32(c.payload + off + 4);
        stts_list.emplace_back(sc, sd);
      }
    }
  });

  if (stsc_list.empty() || chunk_offsets.empty() || sample_count_in_stsz == 0 || stts_list.empty()) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: gpmd track missing stbl child boxes in {}", path);
    std::fclose(fp);
    return nullptr;
  }

  // ---- Build per-sample (file_offset, length, cts, duration). -
  auto demuxer = std::unique_ptr<Mp4GpmfTrackDemuxer>(new Mp4GpmfTrackDemuxer{});
  demuxer->impl_->fp                 = fp;
  demuxer->impl_->timescale          = timescale;
  demuxer->impl_->total_sample_count = sample_count_in_stsz;
  demuxer->impl_->samples.reserve(sample_count_in_stsz);

  // Walk chunks in stco order, assigning samples to chunks per stsc.
  // For each chunk: offset starts at chunk_offsets[chunk_idx]; each
  // sample's file_offset is the running cursor.
  uint32_t sample_no = 0; // 0-indexed; we push samples in stsz order
  for (uint32_t chunk_idx = 0; chunk_idx < chunk_offsets.size(); ++chunk_idx) {
    uint32_t const samples_in_this_chunk = SamplesInChunk(stsc_list, chunk_idx + 1);
    uint64_t cursor = chunk_offsets[chunk_idx];
    for (uint32_t i = 0; i < samples_in_this_chunk; ++i, ++sample_no) {
      if (sample_no >= sample_count_in_stsz) break;
      uint32_t const len = (constant_sample_size != 0u)
                           ? constant_sample_size
                           : sample_sizes[sample_no];
      Impl::SampleEntry entry{
        .file_offset = cursor,
        .length      = len,
        .cts         = 0, // filled below
        .duration    = 0,
      };
      demuxer->impl_->samples.push_back(entry);
      cursor += len;
    }
    if (sample_no >= sample_count_in_stsz) break;
  }
  MBASE_ASSERT(demuxer->impl_->samples.size() == sample_count_in_stsz);

  // Walk stts to assign cumulative DTS (== CTS for metadata) + duration.
  uint64_t cumulative_dts = 0;
  uint32_t s = 0;
  for (auto const& [count, delta] : stts_list) {
    for (uint32_t i = 0; i < count; ++i, ++s) {
      if (s >= demuxer->impl_->samples.size()) break;
      demuxer->impl_->samples[s].cts      = cumulative_dts;
      demuxer->impl_->samples[s].duration = delta;
      cumulative_dts += delta;
    }
    if (s >= demuxer->impl_->samples.size()) break;
  }

  MBASE_LOG_INFO("Mp4GpmfTrackDemuxer: parsed {} gpmd samples (timescale={}) in {}",
    sample_count_in_stsz, timescale, path);
  return demuxer;
}

Mp4GpmfTrackDemuxer::Sample Mp4GpmfTrackDemuxer::GetSampleByDecodeNo(uint32_t sample_no) {
  MBASE_ASSERT(sample_no >= 1 && sample_no <= impl_->total_sample_count);
  auto const& e = impl_->samples[sample_no - 1];

  Sample out;
  out.data.resize(e.length);
  out.cts      = e.cts;
  out.duration = e.duration;

  if (Seek64(impl_->fp, static_cast<int64_t>(e.file_offset), SEEK_SET) != 0) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: fseek failed for sample {}", sample_no);
    out.data.clear();
    return out;
  }
  size_t const read = std::fread(out.data.data(), 1, e.length, impl_->fp);
  if (read != e.length) {
    MBASE_LOG_ERROR("Mp4GpmfTrackDemuxer: short read for sample {} ({} of {} bytes)",
      sample_no, read, e.length);
    out.data.resize(read);
  }
  return out;
}

uint32_t Mp4GpmfTrackDemuxer::total_sample_count() const { return impl_->total_sample_count; }
uint32_t Mp4GpmfTrackDemuxer::timescale()          const { return impl_->timescale; }

} // namespace mdemux
