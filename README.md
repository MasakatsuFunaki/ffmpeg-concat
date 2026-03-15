# Concat Videos

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
