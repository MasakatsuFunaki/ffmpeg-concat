cmake_minimum_required(VERSION 3.15)

# Dependencies are provided by Conan (CMakeDeps + CMakeToolchain):
#   conan export external/ffmpeg
#   conan install . --build=missing
# The generated toolchain is supplied via the CMake preset's toolchainFile.

project(ffmpeg_tools VERSION 2.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8 /Zc:__cplusplus)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

find_package(Boost REQUIRED COMPONENTS program_options log)
find_package(OpenCV REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(indicators REQUIRED)
find_package(ffmpeg-nvenc REQUIRED)

# Platform-specific settings (OS implementation files selected at configure time)
if(WIN32)
    set(PLATFORM_DEFS _WIN32_WINNT=0x0A00)
    set(PLATFORM_LIBS shell32)
    set(PLATFORM_SOURCES
        src/platform/cli_args_win32.cpp
        src/platform/locale_guard_win32.cpp
        src/platform/time_util_win32.cpp)
else()
    set(PLATFORM_DEFS "")
    set(PLATFORM_LIBS pthread)
    set(PLATFORM_SOURCES
        src/platform/cli_args_posix.cpp
        src/platform/locale_guard_posix.cpp
        src/platform/time_util_posix.cpp)
endif()

if(UNIX AND NOT APPLE)
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
endif()

# No ffmpeg binary is staged or shipped: the tools link the static libav*
# libraries from the ffmpeg-nvenc Conan package and run the codecs in-process.

# Levelized static libraries so each tool links only the FFmpeg components it uses.
# Level 1: CLI base, media filesystem, probing, progress reporting (avformat + avcodec + avutil).
add_library(av_base STATIC
    src/tool_app.cpp
    src/common.h
    src/core/arch.h
    src/core/concepts.h
    src/core/scoped_handle.h
    src/core/format_util.h
    src/core/path_util.h
    src/core/profiler.h
    src/core/thread_pool.h
    src/platform/cli_args.h
    src/platform/locale_guard.h
    src/platform/time_util.h
    ${PLATFORM_SOURCES}
    src/av/av_handles.h
    src/av/av_error.cpp src/av/av_error.h
    src/av/media_probe.cpp src/av/media_probe.h
    src/ffmpeg/progress_parser.h
    src/ffmpeg/progress_reporter.cpp src/ffmpeg/progress_reporter.h
    src/media/media_fs.cpp src/media/media_fs.h)
target_compile_features(av_base PUBLIC cxx_std_20)
target_link_libraries(av_base PUBLIC
    Boost::program_options
    Boost::log
    indicators::indicators
    ffmpeg-nvenc::avformat
    ${PLATFORM_LIBS})
target_include_directories(av_base PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(av_base PUBLIC ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(av_base)

# Level 2a: lossless stream-copy remuxing and facade (no avfilter/swscale/swresample).
add_library(av_remux STATIC
    src/av/remuxer.cpp src/av/remuxer.h
    src/ffmpeg/ffmpeg.cpp src/ffmpeg/ffmpeg.h)
target_link_libraries(av_remux PUBLIC av_base)
ffmpeg_tools_set_warnings(av_remux)

# Level 3: frame grabbing, in-memory sampling, and legacy FFmpegUtils facade (+ swscale).
add_library(av_frame STATIC
    src/av/bgr_converter.h
    src/av/frame_grabber.cpp src/av/frame_grabber.h
    src/av/frame_sampler.cpp src/av/frame_sampler.h
    src/ffmpeg/ffmpeg_frame.cpp
    src/common.cpp)
target_link_libraries(av_frame PUBLIC av_remux ffmpeg-nvenc::swscale)
ffmpeg_tools_set_warnings(av_frame)

# Level 2b: codec capability probing and decode/filter/encode pipeline (+ avfilter/swresample/swscale).
add_library(av_transcode STATIC
    src/av/av_capabilities.cpp src/av/av_capabilities.h
    src/av/transcoder.cpp src/av/transcoder.h)
target_link_libraries(av_transcode PUBLIC
    av_base
    ffmpeg-nvenc::avfilter
    ffmpeg-nvenc::swresample
    ffmpeg-nvenc::swscale)
ffmpeg_tools_set_warnings(av_transcode)

# Umbrella interface library preserving backward compatibility for tests and benchmarks.
add_library(common INTERFACE)
add_library(ffmpeg_tools::common ALIAS common)
target_link_libraries(common INTERFACE av_frame av_remux av_transcode)

# Encoder Strategy/Factory library (used by transcode)
add_subdirectory(src/encode)

# Tool executables
add_subdirectory(src/concat)
add_subdirectory(src/transcode)
add_subdirectory(src/cut)
add_subdirectory(src/split)
add_subdirectory(src/extract)

# Tests (off by default; enable with -DBUILD_TESTING=ON)
option(BUILD_TESTING "Build tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarks & profiler tests (off by default; enable with -DBUILD_BENCHMARKS=ON)
option(BUILD_BENCHMARKS "Build Google Benchmark suite" OFF)
if(BUILD_BENCHMARKS OR BUILD_TESTING)
    add_subdirectory(benchmark)
endif()

# ---------------------------------------------------------------------------
# Install target — packages everything for customer delivery into
# "install/win" (Windows) or "install/linux" (Linux).
# ---------------------------------------------------------------------------
if(WIN32)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install/win" CACHE PATH "" FORCE)
else()
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install/linux" CACHE PATH "" FORCE)
endif()

# Executables. These are the whole deliverable now — the codecs are inside them,
# so there is no bin/ffmpeg to ship and nothing to split into .part files.
install(TARGETS concat_videos transcode cut split extract
        RUNTIME DESTINATION .)

# Documentation
install(FILES "${CMAKE_SOURCE_DIR}/README.md"
              "${CMAKE_SOURCE_DIR}/LICENSE"
        DESTINATION .)
