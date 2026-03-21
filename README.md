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

## Releasing

After developing on `dev` branch, run:
```
python release.py
```
This will build the exe, switch to `master`, update the binary, push to GitHub, and switch back to `dev`.
