# FFmpeg Codec Tools

Five fast, self-contained C++20 CLI video tools — `concat_videos`, `transcode`, `cut`, `split`, and `extract` — for **Windows (x64)** and **Linux (x64)**.

Full NLE suites re-encode everything on export even for a simple trim or join, while raw `ffmpeg` filter/remux invocations are verbose and error-prone. **FFmpeg Codec Tools** wraps common video workflows into clean single-command executables with sane defaults:

* **Zero external dependencies** — FFmpeg 7.1.4 (`libavformat`, `libavcodec`, `libavfilter`, `libavutil`, `libswscale`, `libswresample`), `libx264`, `libsvtav1`, `libaom-av1`, and NVIDIA `NVENC`/`CUDA` are **statically linked directly into each executable**. No external `ffmpeg` binary, no DLLs or `.so` files to install, and no subscriptions.
* **Fast** — `concat_videos`, `cut` (default mode), and `split` use lossless stream copy with zero re-encoding; `transcode` uses NVIDIA `NVENC` + `CUDA`/`NVDEC` hardware acceleration when available with automatic CPU fallback.
* **Free & open source** — licensed under GPL-3.0, with no accounts and no telemetry.

---

## Quick Start (Prebuilt Releases)

Download the latest standalone archive for your platform from [**GitHub Releases**](https://github.com/MasakatsuFunaki/ffmpeg-concat/releases):

* **Windows (x64):** `ffmpeg-tools-v2.2.0-windows-x64.zip`
* **Linux (x64):** `ffmpeg-tools-v2.2.0-linux-x64.tar.gz`

Extract the archive anywhere (optionally add the folder to your `PATH`) and run the executables directly — nothing else needs to be installed. Running any tool with **no arguments** or with `--help` / `-h` displays its full CLI usage.

---

## Tools

### `concat_videos`

Concatenates two or more `.mp4` files into a single `.mp4` video **without re-encoding** (lossless stream copy). Input files are sorted chronologically by `YYYY-MM-DD HH-MM-SS` timestamps in their filenames, falling back to each file's last-modified timestamp.

Writes `concatenated_<YYYY-MM-DD_HH-MM-SS>.mp4` into the output directory.

**Options:**

| Flag | Description | Default |
|---|---|---|
| `--input, -i <dir>` | Input directory containing `.mp4` files (at least 2; **required**) | — |
| `--output, -o <dir>` | Output directory for `concatenated_<YYYY-MM-DD_HH-MM-SS>.mp4` (**required**) | — |
| `--help, -h` | Show help message | — |

**Examples:**
```bat
:: Windows
concat_videos.exe -i my_clips -o merged
concat_videos.exe --input "recordings\session1" --output "recordings\joined"
```
```bash
# Linux
./concat_videos -i my_clips -o merged
./concat_videos --input recordings/session1 --output recordings/joined
```

**Benchmark:** 30 GB of `.mp4` recordings concatenated in ~5 minutes on a laptop (13th Gen Intel Core i7-13650HX). Throughput is bounded by SSD I/O, not CPU.

---

### `transcode`

Re-encodes a single `.mp4` file or an entire directory of `.mp4` files into **AV1** or **H.264** (`transcoded_<YYYY-MM-DD_HH-MM-SS>.mp4`).

* **Single-pass multi-file join:** When given a directory with multiple `.mp4` files (sorted by filename date or modification time), joins all segments in a single pass via `libavfilter` `concat`, automatically normalizing mixed frame rates to the lowest segment's FPS.
* **GPU decode & encode:** Automatically uses `CUDA`/`NVDEC` hardware decoding when available on the host (with zero-copy `NVDEC → NVENC` CUDA passthrough for single-file transcodes and transparent software fallback). When `--encoder` is omitted, probes compiled AV1 encoders at runtime and selects the fastest usable one on the machine (`av1_nvenc` → `libsvtav1` → `libaom-av1`).
* **Audio:** Re-encodes audio to **AAC 192 kbps** when present, and supports silent (video-only) `.mp4` files automatically.

**Options:**

| Flag | Description | Default |
|---|---|---|
| `--input, -i <path>` | Input `.mp4` file or directory of `.mp4` files (**required**) | — |
| `--output, -o <dir>` | Output directory for `transcoded_<YYYY-MM-DD_HH-MM-SS>.mp4` (**required**) | — |
| `--crf, -q <0-51>` | Constant quality / CRF (`0` = best, `51` = smallest file) | `30` |
| `--preset, -p <preset>` | Encoder speed/quality preset | auto per encoder |
| `--encoder, -e <encoder>` | Video encoder: `av1_nvenc`, `libsvtav1`, `libaom-av1`, `h264_nvenc`, `libx264` | auto-detect AV1 |
| `--help, -h` | Show help message | — |

**Presets by encoder:**
* `av1_nvenc` / `h264_nvenc` — `p1` (fastest) to `p7` (highest quality), default: `p4`
* `libsvtav1` — `0` (slowest) to `13` (fastest), default: `6`
* `libaom-av1` — `0` (slowest) to `8` (fastest), default: `4`
* `libx264` — `ultrafast` to `veryslow`, default: `medium`

**Examples:**
```bat
:: Windows
transcode.exe -i raw_clips -o encoded
transcode.exe -i raw_clips -o encoded -q 25
transcode.exe -i raw_clips -o encoded -e av1_nvenc -p p7 -q 25
transcode.exe -i raw_clips -o encoded -e h264_nvenc -p p7 -q 20
transcode.exe -i "clips\gameplay.mp4" -o encoded -e libx264 -q 23
```
```bash
# Linux
./transcode -i raw_clips -o encoded
./transcode -i raw_clips -o encoded -q 25
./transcode -i raw_clips -o encoded -e av1_nvenc -p p7 -q 25
./transcode -i raw_clips -o encoded -e h264_nvenc -q 23
./transcode -i clips/gameplay.mp4 -o encoded -e libx264 -q 23
```

**GPU notes:**
* `av1_nvenc` requires an NVIDIA RTX 4000 series (Ada Lovelace) or newer GPU with current drivers.
* `h264_nvenc` requires an NVIDIA GTX 600 series or newer GPU with NVENC driver support.

---

### `cut`

Removes one or more non-overlapping `[start, end]` time intervals from a video and reassembles the remaining segments via **lossless stream copy** (default), or keeps the full timeline and **pixelizes** those intervals (`--blur`).

**Options:**

| Flag | Description | Default |
|---|---|---|
| `--input, -i <file>` | Input video file (**required**) | — |
| `--start, -s <sec...>` | Start time(s) of interval(s) in seconds, space-separated (**required**) | — |
| `--end, -e <sec...>` | End time(s) of interval(s) in seconds, space-separated (**required**) | — |
| `--output, -o <file>` | Output video file path | `<stem>_cut_<ts><ext>` or `<stem>_blur_<ts><ext>` alongside input |
| `--blur, -b` | Pixelize the intervals instead of cutting them out (re-encodes video with `libx264`) | off |
| `--blur-strength <int>` | Pixelization block size when `--blur` is active (higher = coarser blocks) | `30` |
| `--sound-off` | Mute audio during blurred intervals (only with `--blur`) | off |
| `--help, -h` | Show help message | — |

**Modes:**
* **Cut (default):** Extracts the kept gaps via lossless stream copy and concatenates them without re-encoding.
* **Blur (`--blur, -b`):** Preserves full duration, pixelizes each `[start, end]` window via `libavfilter` + `libx264`, and stream-copies audio (or mutes the intervals when `--sound-off` is set).

**Examples:**
```bat
:: Windows
cut.exe -i "videos\recording.mp4" -s 4 -e 10
cut.exe -i "videos\recording.mp4" -s 4 15 -e 8 20
cut.exe -i "videos\recording.mp4" -s 4 -e 10 --blur
cut.exe -i "videos\recording.mp4" -s 4 15 30 -e 8 20 35 --blur --blur-strength 50
cut.exe -i "videos\recording.mp4" -s 3 9 -e 5 11 --blur --sound-off -o "output\censored.mp4"
```
```bash
# Linux
./cut -i videos/recording.mp4 -s 4 -e 10
./cut -i videos/recording.mp4 -s 4 15 -e 8 20
./cut -i videos/recording.mp4 -s 4 -e 10 --blur
./cut -i videos/recording.mp4 -s 3 9 -e 5 11 --blur --sound-off -o output/censored.mp4
```

---

### `split`

Splits a video into `N + 1` sequential parts at `N` split points **without re-encoding** (lossless stream copy). Supports both absolute timestamps from the start of the video and relative durations between splits.

Writes `<stem>_part<1..N>_<YYYY-MM-DD_HH-MM-SS><ext>` into the output directory.

**Options:**

| Flag | Description | Default |
|---|---|---|
| `--input, -i <file>` | Input video file (**required**) | — |
| `--points, -p <sec...>` | Split points in seconds, space-separated (**required**) | — |
| `--relative, -r` | Interpret each split point as relative to the previous split | off (absolute from start) |
| `--output, -o <dir>` | Output directory for `<stem>_part<1..N>_<ts><ext>` | same directory as input |
| `--help, -h` | Show help message | — |

**Modes:**
* **Absolute (default):** `-p 10 30 60` splits at `10s`, `30s`, and `60s` from `0s`.
* **Relative (`--relative, -r`):** `-p 10 20 30 -r` splits at `10s`, `30s` (`10+20`), and `60s` (`30+30`).

**Examples:**
```bat
:: Windows
split.exe -i "videos\lecture.mp4" -p 10 30
split.exe -i "videos\lecture.mp4" -p 15 15 15 -r
split.exe -i "videos\lecture.mp4" -p 10 30 60 -o parts
```
```bash
# Linux
./split -i videos/lecture.mp4 -p 10 30
./split -i videos/lecture.mp4 -p 15 15 15 -r
./split -i videos/lecture.mp4 -p 10 30 60 -o parts
```

---

### `extract`

Extracts JPEG frames, 1920×1080 summary mosaics, and deduplicated text crops from a single video file or every video in a directory (`mp4`, `avi`, `mkv`, `mov`, `wmv`, `flv`, `webm`, `ts`, `m4v`).

**Options:**

| Flag | Description | Default |
|---|---|---|
| `--input, -i <path>` | Input directory of videos or path to a single video file (**required**) | — |
| `--output, -o <dir>` | Output directory for extracted frames (**required**) | — |
| `--image <N>` | Extract `N` equally-spaced frames across each video | — |
| `--image-detect` | Auto-detect up to 100 visually striking frames per video (cached via SHA-256 `.detect_hash`) | off |
| `--mosaic <N> [P]` | Build a 1920×1080 grid mosaic of `N` time-distributed striking frames starting from 1-indexed position `P` (default `1`; requires `--image-detect`; cached via `.mosaic_hash`) | — |
| `--extract-text` | Detect and crop unique Latin/Western text regions via OpenCV MSER + perceptual deduplication | off |
| `--profile` | Print per-stage profiling summary after extraction | off |
| `--help, -h` | Show help message | — |

**Modes (at least one of `--image`, `--image-detect`, or `--extract-text` is required; all modes can run together):**
* **`--image <N>`:** Seeks and decodes `N` evenly-spaced frames across the video duration.
* **`--image-detect`:** Samples up to 500 candidate frames, scores each with OpenCV on sharpness, color saturation, contrast, and edge density, removes near-duplicates, and saves up to 100 time-ordered `striking_XXXX.jpg` frames. Stores a SHA-256 directory fingerprint in `.detect_hash` so repeat runs (e.g. tweaking `--mosaic`) skip re-extraction when frames are intact.
* **`--mosaic <N> [P]`:** Selects `N` evenly-spaced images from detected striking frames starting at 1-indexed position `P` (default `1`), arranges them into a 1920×1080 grid (`mosaic.jpg`), and caches inputs in `.mosaic_hash`.
* **`--extract-text`:** Samples frames at 1 fps, locates character regions using MSER (Maximally Stable Extremal Regions), filters candidate blocks by contrast, edge density, and connected components, and deduplicates crops so repeated subtitles/UI text are saved only once.

**Output structure:**
```text
<output_dir>/
  <video_stem>/
    image/           # --image <N>        -> frame_0001.jpg, ...
    detect/          # --image-detect     -> striking_0001.jpg, ..., .detect_hash
    mosaics/         # --mosaic <N> [P]   -> mosaic.jpg, .mosaic_hash
    text/            # --extract-text     -> text_0001.jpg, ...
```

**Examples:**
```bat
:: Windows
extract.exe -i my_videos -o frames --image 10
extract.exe -i my_videos -o frames --image-detect
extract.exe -i my_videos -o frames --image-detect --mosaic 3
extract.exe -i "clips\movie.mp4" -o frames --image-detect --mosaic 4 20
extract.exe -i my_videos -o frames --extract-text
extract.exe -i my_videos -o frames --image 5 --image-detect --mosaic 4 --extract-text
```
```bash
# Linux
./extract -i my_videos -o frames --image 10
./extract -i my_videos -o frames --image-detect
./extract -i my_videos -o frames --image-detect --mosaic 3
./extract -i clips/movie.mp4 -o frames --image-detect --mosaic 4 20
./extract -i my_videos -o frames --extract-text
./extract -i my_videos -o frames --image 5 --image-detect --mosaic 4 --extract-text
```

---

## Project Layout & Architecture

```text
<repo_root>/
├── external/ffmpeg/   # Local Conan 2 recipe (ffmpeg-nvenc/7.1.4): static libav* + x264/SVT-AV1/aom/NVENC
├── cmake/             # CMake helper modules & build configuration
├── src/
│   ├── common.h/.cpp  # ToolApp base class (Template Method) & FFmpegUtils facade
│   ├── core/          # C++20 concepts, RAII handles, UTF-8 path utils, profiler & jthread pool
│   ├── platform/      # CMake-selected OS abstraction layer (cli_args, locale_guard, time_util)
│   ├── av/            # In-process libavformat/libavcodec/libavfilter/libswscale wrappers & FrameSampler
│   ├── ffmpeg/        # Domain Ffmpeg facade & ProgressReporter (PIMPL indicators progress bars)
│   ├── media/         # Filename date parsing ("YYYY-MM-DD HH-MM-SS"), sorting & timestamp helpers
│   ├── encode/        # EncoderCatalog runtime probe, EncoderProfile strategies & EncodePlan filter builder
│   ├── extract/       # OpenCV frame extractors, scorer, MSER text detector, mosaic & SHA-256 cache
│   ├── concat/        # concat_videos CLI
│   ├── transcode/     # transcode CLI
│   ├── cut/           # cut CLI
│   ├── split/         # split CLI
│   └── extract/       # extract CLI (main entrypoint)
├── benchmark/         # Google Benchmark performance suite + GoogleTest profiler unit tests
├── tests/             # GoogleTest suites labeled by tier: UT (Unit), CT (Component) & ST (System E2E)
├── build.py           # Cross-platform Conan 2 + CMake build, test, benchmark & install driver
└── release.py         # Single-branch main release tagger & local packager
```

---

## Building & Testing from Source

### Prerequisites

* **C++20 compiler:** Visual Studio 2022 (MSVC) on Windows; GCC 13+ on Linux
* **CMake:** 3.15+
* **Conan:** 2.x (`pip install conan`, then run `conan profile detect --force` once if you do not have a default profile)
* **Python:** 3.8+

> **First-time build note:** The initial build compiles static FFmpeg 7.1.4 + `libx264` + `libsvtav1` + `libaom-av1` + `ffnvcodec` (`external/ffmpeg`) and a minimal static OpenCV (`with_ffmpeg=False`) into your local Conan cache (~20–60 minutes depending on CPU cores). Every subsequent build reuses the Conan cache and completes in seconds.

### Recommended (`build.py`)

```bash
# Build all 5 Release executables
python build.py

# Configure with tests enabled, build, and run the full CTest suite (UT + CT + ST)
python build.py --test

# Run only Unit Tests (UT), Component Tests (CT), or System Tests (ST)
python build.py --tier UT
python build.py --tier CT
python build.py --tier ST

# Build and run the Google Benchmark performance suite (+ per-stage profiler summary)
python build.py --benchmark
python build.py --benchmark -- --profile-summary

# Stage standalone deliverables into install/win/ (Windows) or install/linux/ (Linux)
python build.py --install
```

### Manual Conan 2 + CMake Presets

```bash
# 1. Export the local static FFmpeg recipe and resolve dependencies
conan export external/ffmpeg
conan install . --build=missing
```

**Windows (PowerShell / Developer Command Prompt):**
```powershell
cmake --preset conan-windows-release -DBUILD_TESTING=ON
cmake --build --preset conan-windows-release
ctest --preset conan-windows-release --output-on-failure
```

**Linux:**
```bash
cmake --preset conan-linux-release -DBUILD_TESTING=ON
cmake --build --preset conan-linux-release
ctest --preset conan-linux-release --output-on-failure
```

To filter by test tier directly with `ctest`, append `-L UT`, `-L CT`, or `-L ST`.

---

## Releasing

Releases follow a single-branch workflow on `main`:

```bash
# Tag an explicit version on main and push main + the annotated tag to GitHub
python release.py --tag v2.2.0

# Or automatically bump the latest semver tag (patch | minor | major)
python release.py --bump minor
```

Pushing the annotated `v*` tag triggers [`.github/workflows/release.yml`](.github/workflows/release.yml), which builds, runs the `UT`/`CT`/`ST` test suites, verifies standalone static linkage on both **Windows (x64)** and **Linux (x64)**, and publishes `ffmpeg-tools-<tag>-windows-x64.zip`, `ffmpeg-tools-<tag>-linux-x64.tar.gz`, and `SHA256SUMS.txt` to GitHub Releases.

For local packaging or uploading a host archive directly via the GitHub CLI (`gh`), run `python release.py --local`.

---

## License

Licensed under the **GNU General Public License v3.0 (GPL-3.0)** — see [`LICENSE`](LICENSE) for details.
