# Project Guidelines

## Architecture

**FFmpeg Codec Tools (`v2.0.0`)** — 5 self-contained C++17 CLI video executables (`concat_videos`, `transcode`, `cut`, `split`, `extract`) for **Windows (x64)** and **Linux (x64)**.

FFmpeg 7.1.4 (`libavformat`, `libavcodec`, `libavfilter`, `libavutil`, `libswscale`, `libswresample`), `libx264`, `libsvtav1`, `libaom-av1`, and NVIDIA `NVENC`/`CUDA` (`ffnvcodec`) are **statically linked directly into every executable** via the local Conan 2 recipe in `external/ffmpeg/`. No external `ffmpeg` binary is spawned, staged, or shipped.

### Directory Layout

- `external/ffmpeg/` — Local Conan 2 recipe (`ffmpeg-nvenc/7.1.4`) producing static `libav*` libraries + codecs.
- `cmake/` — Modular CMake helper scripts included by `CMakeLists.txt`.
- `src/common.h` / `src/common.cpp` — `ToolApp` base class (GoF Template Method: `name()`, `define_options()`, `execute()`, optional `extra_help()`) and `FFmpegUtils` compatibility facade.
- `src/av/` — Low-level in-process `libav*` wrappers (`media_probe`, `remuxer`, `frame_grabber`, `transcoder`, `av_capabilities`, `av_error`). Keep raw `libav*` headers insulated inside `src/av/`.
- `src/ffmpeg/` — Domain `Ffmpeg` facade (`duration`, `frame_rate`, `extract_frame`, `run_segment`, `concat`) and `ProgressReporter` strategy (`IndicatorsProgressReporter`).
- `src/media/` — Filesystem and timestamp utilities (`media::file_date`, `media::current_timestamp`, `media::collect_mp4_files`, `media::collect_video_files`, `media::log_output_size`).
- `src/encode/` — `EncoderCatalog` (runtime encoder session probing), `EncoderProfile` strategies (`NvencProfile`, `SvtAv1Profile`, `LibaomProfile`, `X264Profile`), and `EncodePlan` (`libavfilter` `concat` graph generation + mixed-FPS normalization).
- `src/extract/` — `extractcore` library: `FrameExtractor` strategies (`EquallySpacedExtractor`, `StrikingFrameExtractor`, `TextCropExtractor`), `FrameScorer`, `MosaicBuilder`, MSER `TextDetector`, `CropDeduplicator`, and SHA-256 `FingerprintCache` (`.detect_hash`, `.mosaic_hash`).
- `src/{concat,transcode,cut,split,extract}/` — Thin CLI `ToolApp` subclasses (`main()` guarded by `#ifndef TESTING_NO_MAIN`).
- `tests/` — GoogleTest suites linking `*_lib` targets (`TESTING_NO_MAIN`), labeled by CTest tier: `UT` (fast unit tests) and `CT` (component/integration tests).

### Adding a New Tool

1. Create `src/<tool>/<tool>.h`, `<tool>.cpp` (wrap `main()` in `#ifndef TESTING_NO_MAIN`), and `CMakeLists.txt`.
2. Inherit from `ToolApp` (`src/common.h`) and implement `name()`, `define_options()`, and `execute()`.
3. Delegate media operations to `Ffmpeg` (`src/ffmpeg/ffmpeg.h`), `av::*` (`src/av/`), and `media::*` (`src/media/media_fs.h`) — never shell out to an external `ffmpeg` process.
4. Register `add_subdirectory(src/<tool>)` and `install(TARGETS ...)` in root `CMakeLists.txt`, and add `<tool>_lib` + `UT`/`CT`-labeled tests under `tests/`.

## Code Style

- **C++17** (`CMAKE_CXX_STANDARD 17`), `#pragma once` in headers, and **PIMPL** where appropriate to keep implementation details and heavy third-party headers out of public interfaces.
- `snake_case` for functions and variables; `PascalCase` for classes and structs.
- `BOOST_LOG_TRIVIAL(info/warning/error/debug)` for all logging — never `std::cout` or `std::cerr`.
- `std::filesystem` (aliased as `namespace fs = std::filesystem`) for all path handling.
- `Boost.ProgramOptions` for CLI flags (`--help, -h` and zero-argument help output are handled centrally in `ToolApp::run()`).
- Format floating-point numbers passed into `libavfilter` strings using `std::locale::classic()` (or exact rationals such as `30/1`) because libraries like `indicators` can alter `LC_NUMERIC` at runtime.
- Return `bool`/`int` status codes, keep `av::*` entrypoints exception-free, and log actionable errors before returning non-zero.
- Guard OS-specific code with `#ifdef _WIN32`.

## Build, Test, and Release

Dependencies (`boost/1.84.0`, `gtest/1.14.0`, `opencv/4.9.0`, `openssl/3.2.1`, `indicators/2.3`, and local `ffmpeg-nvenc/7.1.4`) are managed with **Conan 2.x** (`conanfile.py`). OpenCV is built with `with_ffmpeg=False`, `with_quirc=False`, and GUI/Wayland backends disabled so it never conflicts with static `ffmpeg-nvenc`.

```bash
# Primary driver (exports external/ffmpeg, runs conan install, configures & builds)
python build.py                  # Release build
python build.py --test           # Build with BUILD_TESTING=ON and run all CTest suites
python build.py --tier UT        # Run Unit Tests only (-L UT)
python build.py --tier CT        # Run Component Tests only (-L CT)
python build.py --install        # Stage standalone binaries into install/win/ or install/linux/

# Manual Conan 2 + CMake preset workflow
conan export external/ffmpeg
conan install . --build=missing
cmake --preset conan-windows-release -DBUILD_TESTING=ON   # or conan-linux-release
cmake --build --preset conan-windows-release              # or conan-linux-release
ctest --preset conan-windows-release --output-on-failure  # add -L UT or -L CT to filter
```

- **Single-branch `main` releases:** `python release.py --tag v2.0.0` (or `--bump patch|minor|major`, or `--local`) pushes `main` and the annotated tag so `.github/workflows/release.yml` builds, tests, verifies, and attaches Windows (`.zip`) + Linux (`.tar.gz`) archives and `SHA256SUMS.txt` to GitHub Releases.

## CLI & Repository Conventions

- **No `-f` flag anywhere** and **no implicit `input/` or `output/` default directories**:
  - `--input, -i` is **required** on all 5 tools.
  - `--output, -o` is **required** on `concat_videos`, `transcode`, and `extract`, and **optional** on `cut` (defaults alongside the input file as `<stem>_cut_<ts><ext>` or `<stem>_blur_<ts><ext>`) and `split` (defaults to the input file's directory as `<stem>_part<1..N>_<ts><ext>`).
- Input video filenames containing `"YYYY-MM-DD HH-MM-SS"` are parsed and sorted chronologically (falling back to last-write time); generated output filenames use `"YYYY-MM-DD_HH-MM-SS"`.
- Test media fixtures live in `tests/input/` (staged into `${CMAKE_BINARY_DIR}/tests/input` by CMake).
- Never commit build outputs, release archives, or personal/machine-local filesystem paths; always use generic placeholders like `<repo_root>` or `<install>` in documentation.
- See [README.md](../README.md) for complete per-tool flags and examples.
