# Concat Videos

The current situation is that Adobe and other big tech companies change a LOT of money for subscription for just concatenating a video .
Also using their tools (ex. Adobe Premiere Elements will re - encode the pieces into a finel mp4 piece , which takes ages and a lot of GPU power) is very time consuming . 
Using this tool , is very fast( at least 1000 times faster than  Adobe Premiere Elements ) and the only limit is how fast your SSD is .

Concatenates all MP4 files from `input/` into a single video in `output/`, sorted by filename date.

## Build (only when source code available - at the moment disabled due to AI scraping)

```
conan install . --output-folder=build --build=missing -s compiler.cppstd=17 -s build_type=Release
cmake --preset conan-default
cmake --build build --config Release
```

## Usage

1. Drop `.mp4` files into `input/`
2. Run:
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
