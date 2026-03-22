# FFmpeg Codec Tools (Windows)

A collection of fast, lightweight command-line video tools powered by FFmpeg. No subscriptions, no bloat — just drop your files and run.

**Why not use Adobe Premiere / other editors?**
* Big tech charges expensive recurring fees for basic video operations.
* Editors like Premiere Elements re-encode everything by default, wasting GPU power and time even when no edits are made.

**Why these tools?**
* **Blazing fast** — concat uses stream copy (no re-encoding), transcode uses NVIDIA hardware acceleration when available.
* **Hardware efficient** — concat is limited only by your SSD speed; transcode offloads to your GPU.
* **Simple** — drop MP4 files into `input/`, run the exe, get your output.

## Build (only when source code available - at the moment disabled due to AI scraping)

```
conan install . --output-folder=build --build=missing -s compiler.cppstd=17 -s build_type=Release
cmake --preset conan-default
cmake --build build --config Release
```

---

## Tools

### concat_videos.exe

Concatenates multiple MP4 files into a single video **without re-encoding** (stream copy). Files are sorted by the date in their filename.

**Parameters:**

| Flag | Description | Default |
|---|---|---|
| `-i <dir>` | Input directory | `input` |
| `-o <dir>` | Output directory | `output` |
| `-f <path>` | FFmpeg path | `bin/ffmpeg.exe` |
| `-h` | Show help | |

**Example:**
```
# Drop files into input/: "2026-03-15 16-33-43.mp4", "2026-03-15 16-33-51.mp4"
.\build\Release\concat_videos.exe
.\build\Release\concat_videos.exe -i my_clips -o merged
```

**Benchmark:** 30 GB of videos concatenated in ~5 minutes on a laptop (13th Gen i7-13650HX). The bottleneck is SSD speed, not CPU.

---

### transcode.exe

Concatenates (if multiple files) and re-encodes MP4 files to **AV1** format. Automatically detects the best available encoder: NVIDIA NVENC GPU → SVT-AV1 CPU → libaom CPU.

**Parameters:**

| Flag | Description | Default |
|---|---|---|
| `-i <dir>` | Input directory | `input` |
| `-o <dir>` | Output directory | `output` |
| `-f <path>` | FFmpeg path | `bin/ffmpeg.exe` |
| `-q <0-51>` | Quality / CRF (lower = better) | `30` |
| `-p <preset>` | Encoder preset | auto |
| `-e <encoder>` | Force encoder (`av1_nvenc`, `libsvtav1`, `libaom-av1`) | auto-detect |
| `-h` | Show help | |

**Presets by encoder:**
* `av1_nvenc` — `p1` (fastest) to `p7` (best quality), default: `p4`
* `libsvtav1` — `0` (slowest) to `13` (fastest), default: `6`
* `libaom-av1` — `0` (slowest) to `8` (fastest), default: `4`

**Examples:**
```
# Basic: concat + encode to AV1 with defaults
.\build\Release\transcode.exe

# Higher quality
.\build\Release\transcode.exe -q 25

# Force NVIDIA GPU encoding with best quality preset
.\build\Release\transcode.exe -e av1_nvenc -p p7 -q 25

# Custom folders
.\build\Release\transcode.exe -i raw_clips -o encoded
```

**Note:** `av1_nvenc` requires an NVIDIA RTX 4000 series (Ada Lovelace) or newer GPU.

---

### cut.exe

Cuts out one or more intervals from a video **without re-encoding** (stream copy) and reassembles the remaining parts. Alternatively, pixelizes the intervals instead of removing them.

**Parameters:**

| Flag | Description | Default |
|---|---|---|
| `-i <file>` | Input video file (required) | |
| `-s <sec...>` | Start(s) of interval(s) in seconds, space-separated (required) | |
| `-e <sec...>` | End(s) of interval(s) in seconds, space-separated (required) | |
| `-o <file>` | Output file path | auto-generated in input directory |
| `-f <path>` | FFmpeg path | `bin/ffmpeg.exe` |
| `--blur, -b` | Apply pixelized blur instead of cutting | off |
| `--blur-strength` | Pixelization block size (higher = more pixelated) | `30` |
| `--sound-off` | Mute audio during blurred intervals (only with `--blur`) | off |
| `-h` | Show help | |

**Modes:**
* **Cut (default):** Removes the intervals and concatenates the remaining parts using stream copy (no re-encoding).
* **Blur (`--blur`):** Keeps the full video but pixelizes the intervals. Requires re-encoding.

**Examples:**
```
# Cut out seconds 4–10 from a video
.\build\Release\cut.exe -i "input\my_video.mp4" -s 4 -e 10

# Cut out multiple intervals: [4,8] and [15,20]
.\build\Release\cut.exe -i "input\my_video.mp4" -s 4 15 -e 8 20

# Pixelize seconds 4–10 instead of removing them
.\build\Release\cut.exe -i "input\my_video.mp4" -s 4 -e 10 --blur

# Pixelize multiple intervals
.\build\Release\cut.exe -i "input\my_video.mp4" -s 4 15 30 -e 8 20 35 --blur

# Pixelize with stronger effect (larger blocks)
.\build\Release\cut.exe -i "input\my_video.mp4" -s 4 -e 10 --blur --blur-strength 50

# Pixelize and save to specific path
.\build\Release\cut.exe -i "input\my_video.mp4" -s 4 -e 10 --blur -o "output\censored.mp4"

# Pixelize multiple intervals and mute audio during those parts
.\build\Release\cut.exe -i "input\my_video.mp4" -s 3 9 -e 5 11 --blur --sound-off
```

---

### split.exe

Splits a video into multiple parts at given time points **without re-encoding** (stream copy). Supports both absolute and relative split points.

**Parameters:**

| Flag | Description | Default |
|---|---|---|
| `-i <file>` | Input video file (required) | |
| `-p <sec...>` | Split points in seconds, space-separated (required) | |
| `-r` | Treat split points as relative to the previous split | absolute |
| `-o <dir>` | Output directory | same as input file |
| `-f <path>` | FFmpeg path | `bin/ffmpeg.exe` |
| `-h` | Show help | |

**Modes:**
* **Absolute (default):** Split points are offsets from the start of the video.
* **Relative (`-r`):** Each split point is relative to the previous one (e.g., `-p 10 20 30` → splits at 10s, 30s, 60s).

**Examples:**
```
# Split at 10s and 30s → 3 parts: [0,10], [10,30], [30,end]
.\build\Release\split.exe -i "input\my_video.mp4" -p 10 30

# Split into 4 equal-ish parts using relative durations of 15s each
.\build\Release\split.exe -i "input\my_video.mp4" -p 15 15 15 -r

# Split and save parts to output directory
.\build\Release\split.exe -i "input\my_video.mp4" -p 10 30 60 -o output
```

---

## Testing

The project includes Google Test suites for each tool. Test data (a short MP4 video) lives in `tests/input/` and is automatically copied into the build directory at configure time. All test outputs go to `build/tests/output/`.

**Run all tests (from project root):**
```
ctest --test-dir build --build-config Release --output-on-failure
```

**Run a single test suite:**
```
ctest --test-dir build --build-config Release -R test_cut --output-on-failure
```

**Or run a test executable directly (with verbose GTest output):**
```
.\build\Release\test_common.exe
.\build\Release\test_concat.exe
.\build\Release\test_transcode.exe
.\build\Release\test_cut.exe
.\build\Release\test_split.exe
```

| Suite | Tests | What it covers |
|---|---|---|
| `test_common` | 11 | `current_timestamp`, `get_file_date`, `ensure_ffmpeg`, `collect_mp4_files`, `get_video_duration`, `run_ffmpeg_segment` |
| `test_concat` | 4 | Help flag, missing/invalid input, actual 2-file concat |
| `test_transcode` | 3 | Help flag, missing/invalid input |
| `test_cut` | 7 | Help flag, missing input, basic cut, blur, multi-interval, blur + sound-off |
| `test_split` | 6 | Help flag, missing input, single/multi split (absolute), relative mode |

---

## Releasing

After developing on `dev` branch, run:
```
python release.py
```
This will build the exe, switch to `master`, update the binary, push to GitHub, and switch back to `dev`.
