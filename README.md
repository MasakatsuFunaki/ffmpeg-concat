# Concat Videos (Windows platforms only)

**The Problem:**

* **High Costs:** Big tech companies (like Adobe) charge expensive, recurring subscription fees for basic features like concatenating videos.
* **Wasted Time & Resources:** Traditional tools like Adobe Premiere Elements force the video to re-encode into a final MP4. This drains massive amounts of GPU power and takes ages.

**The Solution:**

* **Blazing Fast:** This tool bypasses the re-encoding process, making it at least 1,000 times faster than Premiere Elements. 
* **Hardware Efficient:** The process is so lightweight that your only speed limit is your SSD.

*Note: Concatenates all MP4 files from `input/` into a single video in `output/`, sorted by filename date.*

## Usage

1. Drop `.mp4` files into `input/` (example names : 2026-03-15 16-33-43.mp4 , 2026-03-15 16-33-51.mp4)
3. Run:
```
.\build\Release\concat_videos.exe
```

### Options

```
-i <dir>    Input directory  (default: input)
-o <dir>    Output directory (default: output)
-f <path>   FFmpeg path      (default: bin/ffmpeg.exe)
-h          Show help
```
### Benchmark
30 GB of multiple videos has been concatenated into a final file in  aprox 5 minutes on a laptop CPU:	13th Gen Intel(R) Core(TM) i7-13650HX, 2600 Mhz, 14 Core(s), 20 Logical Processor(s)
