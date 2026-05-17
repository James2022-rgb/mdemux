# mdemux

MP4 / ISOBMFF demuxer. Wraps L-SMASH behind a small C++ surface
that hides L-SMASH from public headers entirely (pimpl). One
class per stream type (HEVC video, AAC audio) -- each opens the
file independently, surfaces only the bytes a downstream decoder
needs, and exposes the per-track metadata (timing, color, HDR
static metadata, rotation, etc.) the player layer needs to
configure its render path.

## Language

All code comments MUST be written in English.

## Symbols

It is forbidden to use the full width forms of symbols that have
counterparts in ASCII. e.g. `()`, `:`, `,`, `0-9`.

## Coding style

- `m<short>` library naming.
- `src/<libname>/{public,private}` layout; public include path is
  reached as `#include "mdemux/public/<file>.h"`.
- TU header (`#include "<this file>.h"`) listed first in each
  `.cpp`, then C++ system headers, then external headers, then
  public project headers, then private project headers -- each
  group separated by a blank line.
- Slabs of related includes are grouped and labeled with the
  same `// public project headers --...` style markers used in
  the existing files.
- Public headers MUST NOT include `lsmash.h`. L-SMASH state is
  hidden behind a pimpl `struct Impl` defined in the `.cpp`.

## Build

CMake-based C++23 static library. Typically consumed as a
sibling-lib `add_subdirectory(... mdemux ...)` from a parent
project's `CMakeLists.txt`. The parent **MUST** add `mbase`
first -- mdemux's `CMakeLists.txt` asserts the target already
exists (no own `add_subdirectory` for it, so the parent stays
in control of which version is used).

Target name: `mdemux`. Links `mbase` PUBLIC, `lsmash` PRIVATE.

When adding source files to `CMakeLists.txt`, list them in
alphabetical order within each `set(...)` block.

## Directory structure

```
src/mdemux/
  public/   <- API surface (no L-SMASH leaks)
    mp4_hevc_video_demuxer.h
    mp4_aac_audio_demuxer.h
  private/  <- implementations (touch L-SMASH freely)
    mp4_hevc_video_demuxer.cpp
    mp4_aac_audio_demuxer.cpp
```

## Public API

- `Mp4HevcVideoDemuxer` -- one HEVC video track of an MP4 /
  ISOBMFF file. Surfaces:
  - raw `hvcC` bytes (for the consumer's HEVC decoder
    configuration record parser; mdemux does NOT parse hvcC).
  - per-decode-order sample bytes via `GetSampleByDecodeNo(n)`
    (1-indexed, MP4 form -- length-prefixed NAL units, NOT
    Annex B).
  - `DisplayToDecodeNo(display_idx)` permutation built from
    the track's (DTS, CTS) timestamp list at Open time --
    identity for IDR/P-only streams, non-trivial for B-frame
    streams.
  - `PriorIrapSampleNo(decode_no)` O(log N) lookup over a
    pre-indexed IRAP set (no per-call sample re-fetch).
  - per-track metadata: timescale, display PTS in seconds,
    `Mp4ColorInfo` (`colr` box), `Mp4HdrStaticMetadata` (`mdcv` +
    `clli`), rotation degrees (from `tkhd` matrix; cardinal
    rotations 0/90/180/270 only).
  - Multi-HEVC-track files (e.g. GoPro Max 2 `.360` carries two
    HEVC tracks): pass `video_track_index` to `Open`; use
    `CountHevcVideoTracks(path)` to discover how many there are.
- `Mp4AacAudioDemuxer` -- sibling for the first AAC audio
  track. Surfaces the AAC AudioSpecificConfig (ASC) bytes from
  `esds`, per-sample raw AAC AU bytes, and stream descriptors
  (sample rate / channel count / timescale / sample count).
  No display-order permutation (AAC has no B-frame reorder).
- `CountHevcVideoTracks(path)` -- free function helper for the
  multi-track case.

The public types live in namespace `mdemux`.

## Dependencies

- `mbase` -- assertions, logging. Linked PUBLIC because public
  headers use `MBASE_*` macros transitively (via pimpl in the
  `.cpp`s it's PRIVATE per-translation-unit, but the parent
  build benefits from having mbase visible). Provided by the
  parent; mdemux asserts the target exists.
- `L-SMASH` (`l-smash/l-smash` master) -- MP4 / ISOBMFF library,
  pulled in via FetchContent inside this lib's `CMakeLists.txt`.
  Linked PRIVATE; consumers never see L-SMASH headers because
  the public types use pimpl. L-SMASH ships only with autotools
  and a Visual Studio solution (no CMakeLists.txt of its own),
  so the FetchContent block enumerates its sources explicitly
  -- update the file list if upstream adds or removes `.c`
  files under `common/`, `core/`, `codecs/`, or `importer/`.

## License

L-SMASH is ISC-licensed (permissive, attribution required).
mdemux itself adds no extra license constraint beyond mbase
and the project the consumer chooses.

## Threading

L-SMASH's `lsmash_root_t` is **not** internally thread-safe.
Each demuxer instance owns its own root, so:
- Multiple instances of `Mp4HevcVideoDemuxer` / `Mp4AacAudioDemuxer`
  -- even pointing at the same file -- can be used from
  different threads concurrently (they each have a private
  root).
- Calls on the **same** demuxer instance must be serialized
  by the caller. The mutating call to watch for is
  `GetSampleByDecodeNo`; the others (`total_sample_count` etc.)
  return cached values built once at Open and are safe to read
  from any thread.

## Commit messages

Conventional-Commits-style with the lib as the scope, e.g.
`feat(mdemux): ...`, `fix(mdemux): ...`, `perf(mdemux): ...`,
`docs(mdemux): ...`, `refactor(mdemux): ...`.
