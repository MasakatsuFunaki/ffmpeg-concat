# FFmpeg Codec Tools

Fast, lightweight CLI video tools powered by FFmpeg. No subscriptions, no re-encoding overhead — just drop files and run. Supports **Windows** and **Linux**.

Full editors like Premiere Pro (~$276/year) or DaVinci Resolve re-encode everything on export, even for a simple concat. Raw FFmpeg commands are verbose and error-prone. These tools wrap the common operations into single commands with sane defaults.

* **Fast** — concat/cut/split use stream copy; transcode uses NVIDIA NVENC when available
* **Simple** — drop MP4s into `input/`, run the tool, get output
* **Free & open source** — no accounts, no telemetry


## Tools

### concat_videos

Concatenates multiple MP4 files into a single video **without re-encoding** (stream copy). Files are sorted by the date in their filename.

**Parameters:**

| Flag | Description | Default |
|---|---|---|
| `-i <dir>` | Input directory | `input` |
| `-o <dir>` | Output directory | `output` |
| `-f <path>` | FFmpeg path | `bin/ffmpeg.exe` (Win) / `bin/ffmpeg` (Linux) |
| `-h` | Show help | |

**Example:**
```bat
# Windows — drop files into input/: "2026-03-15 16-33-43.mp4", "2026-03-15 16-33-51.mp4"
concat_videos.exe
concat_videos.exe -i my_clips -o merged
```
```bash
# Linux
./concat_videos
./concat_videos -i my_clips -o merged
```

**Benchmark:** 30 GB of videos concatenated in ~5 minutes on a laptop (13th Gen i7-13650HX). The bottleneck is SSD speed, not CPU.

---

### transcode

Concatenates (if multiple files) and re-encodes MP4 files to **AV1** format. Automatically detects the best available encoder: NVIDIA NVENC GPU → SVT-AV1 CPU → libaom CPU.

**Parameters:**

| Flag | Description | Default |
|---|---|---|
| `-i <dir>` | Input directory | `input` |
| `-o <dir>` | Output directory | `output` |
| `-f <path>` | FFmpeg path | `bin/ffmpeg.exe` (Win) / `bin/ffmpeg` (Linux) |
| `-q <0-51>` | Quality / CRF (lower = better) | `30` |
| `-p <preset>` | Encoder preset | auto |
| `-e <encoder>` | Force encoder (`av1_nvenc`, `libsvtav1`, `libaom-av1`) | auto-detect |
| `-h` | Show help | |

**Presets by encoder:**
* `av1_nvenc` — `p1` (fastest) to `p7` (best quality), default: `p4`
* `libsvtav1` — `0` (slowest) to `13` (fastest), default: `6`
* `libaom-av1` — `0` (slowest) to `8` (fastest), default: `4`

**Examples:**
```bat
# Windows
transcode.exe
transcode.exe -q 25
transcode.exe -e av1_nvenc -p p7 -q 25
transcode.exe -i raw_clips -o encoded
```
```bash
# Linux
./transcode
./transcode -q 25
./transcode -e av1_nvenc -p p7 -q 25
./transcode -i raw_clips -o encoded
```

**Note:** `av1_nvenc` requires an NVIDIA RTX 4000 series (Ada Lovelace) or newer GPU.

---

### cut

Cuts out one or more intervals from a video **without re-encoding** (stream copy) and reassembles the remaining parts. Alternatively, pixelizes the intervals instead of removing them.

**Parameters:**

| Flag | Description | Default |
|---|---|---|
| `-i <file>` | Input video file (required) | |
| `-s <sec...>` | Start(s) of interval(s) in seconds, space-separated (required) | |
| `-e <sec...>` | End(s) of interval(s) in seconds, space-separated (required) | |
| `-o <file>` | Output file path | auto-generated in input directory |
| `-f <path>` | FFmpeg path | `bin/ffmpeg.exe` (Win) / `bin/ffmpeg` (Linux) |
| `--blur, -b` | Apply pixelized blur instead of cutting | off |
| `--blur-strength` | Pixelization block size (higher = more pixelated) | `30` |
| `--sound-off` | Mute audio during blurred intervals (only with `--blur`) | off |
| `-h` | Show help | |

**Modes:**
* **Cut (default):** Removes the intervals and concatenates the remaining parts using stream copy (no re-encoding).
* **Blur (`--blur`):** Keeps the full video but pixelizes the intervals. Requires re-encoding.

**Examples:**
```bat
# Windows
cut.exe -i "input\my_video.mp4" -s 4 -e 10
cut.exe -i "input\my_video.mp4" -s 4 15 -e 8 20
cut.exe -i "input\my_video.mp4" -s 4 -e 10 --blur
cut.exe -i "input\my_video.mp4" -s 4 15 30 -e 8 20 35 --blur
cut.exe -i "input\my_video.mp4" -s 4 -e 10 --blur --blur-strength 50
cut.exe -i "input\my_video.mp4" -s 4 -e 10 --blur -o "output\censored.mp4"
cut.exe -i "input\my_video.mp4" -s 3 9 -e 5 11 --blur --sound-off
```
```bash
# Linux
./cut -i "input/my_video.mp4" -s 4 -e 10
./cut -i "input/my_video.mp4" -s 4 15 -e 8 20
./cut -i "input/my_video.mp4" -s 4 -e 10 --blur
./cut -i "input/my_video.mp4" -s 3 9 -e 5 11 --blur --sound-off
```

---

### split

Splits a video into multiple parts at given time points **without re-encoding** (stream copy). Supports both absolute and relative split points.

**Parameters:**

| Flag | Description | Default |
|---|---|---|
| `-i <file>` | Input video file (required) | |
| `-p <sec...>` | Split points in seconds, space-separated (required) | |
| `-r` | Treat split points as relative to the previous split | absolute |
| `-o <dir>` | Output directory | same as input file |
| `-f <path>` | FFmpeg path | `bin/ffmpeg.exe` (Win) / `bin/ffmpeg` (Linux) |
| `-h` | Show help | |

**Modes:**
* **Absolute (default):** Split points are offsets from the start of the video.
* **Relative (`-r`):** Each split point is relative to the previous one (e.g., `-p 10 20 30` → splits at 10s, 30s, 60s).

**Examples:**
```bat
# Windows
split.exe -i "input\my_video.mp4" -p 10 30
split.exe -i "input\my_video.mp4" -p 15 15 15 -r
split.exe -i "input\my_video.mp4" -p 10 30 60 -o output
```
```bash
# Linux
./split -i "input/my_video.mp4" -p 10 30
./split -i "input/my_video.mp4" -p 15 15 15 -r
./split -i "input/my_video.mp4" -p 10 30 60 -o output
```

---

### extract

Extracts image frames from all videos in a directory. Three modes: equally-spaced sampling, AI-powered auto-detection of visually striking frames using OpenCV (scored by sharpness, color saturation, contrast, and edge density), and text extraction that detects and crops unique text regions (Latin/Western) from frames.

**Parameters:**

| Flag | Description | Default |
|---|---|---|
| `-i <dir>` | Input directory containing video files (required) | `input` |
| `-o <dir>` | Output directory for extracted frames | `output` |
| `-f <path>` | FFmpeg path | `bin/ffmpeg.exe` (Win) / `bin/ffmpeg` (Linux) |
| `--image <N>` | Extract N equally-spaced frames per video | |
| `--image-detect` | Auto-detect visually striking frames (max 100 per video) | |
| `--extract-text` | Extract unique text crops from frames (Latin/Western text) | |
| `-h` | Show help | |

**Modes:**
* **`--image N`:** Extracts exactly N frames spaced equally across the video duration. Fast — just FFmpeg seeks.
* **`--image-detect`:** Samples up to 500 candidate frames, then scores each one with OpenCV on 4 metrics (sharpness, color saturation, contrast, edge density). Keeps the top 100 most visually striking frames.
* **`--extract-text`:** Samples frames across the video, detects text regions using MSER (Maximally Stable Extremal Regions), groups nearby characters into text blocks, and crops only the text rectangles. Deduplicates crops so each saved image contains unique text not seen in previous frames. Ideal for extracting subtitles, dialogue boxes, book pages, UI text, etc.
* All modes can be used **simultaneously**. Output goes into separate subdirectories.

**Supported video formats:** mp4, avi, mkv, mov, wmv, flv, webm, ts, m4v

**Output structure:**
```
output/
  <video_stem>/
    image/           ← equally-spaced frames (frame_0001.jpg, ...)
    detect/          ← striking frames (striking_0001.jpg, ...)
    text/            ← unique text crops (text_0001.jpg, ...)
```

**Examples:**
```bat
# Windows
extract.exe --image 10
extract.exe --image-detect
extract.exe --extract-text
extract.exe --image 5 --image-detect --extract-text -i my_videos -o my_frames
extract.exe --image 20 -i recordings -o thumbnails -f bin\ffmpeg.exe
```
```bash
# Linux
./extract --image 10
./extract --image-detect
./extract --extract-text
./extract --image 5 --image-detect --extract-text -i my_videos -o my_frames
```

---

## Releasing

After developing on `dev` branch, run:
```bash
python3 release.py
```
This will build, copy artifacts to `build/release/<os>/`, switch to `master`, push to GitHub, and switch back to `dev`.

---

## Building from Source

### Windows

**Prerequisites:** Visual Studio 2022, CMake 3.15+, Conan 2.x

```bat
python switch_build.py
conan install . --output-folder=build --build=missing
cmake --preset conan-default
cmake --build build --config Release
```

**Debug build:**
```bat
cmake --build build --config Debug
```

**Run tests:**
```bat
cmake --preset conan-default -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

### Linux

**Prerequisites:** GCC 9+, CMake 3.15+, Conan 2.x (`pip install conan`), `curl`, `tar`

```bash
python3 switch_build.py
conan install . --output-folder=build_linux --build=missing
cmake --preset conan-release
cmake --build --preset conan-release
```

**Run tests:**
```bash
cmake --preset conan-release -DBUILD_TESTING=ON
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
```
