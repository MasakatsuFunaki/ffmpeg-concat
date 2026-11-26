cmake_minimum_required(VERSION 3.15)

# Dependencies are provided by Conan (CMakeDeps + CMakeToolchain):
#   conan export external/ffmpeg
#   conan install . --build=missing
# The generated toolchain is supplied via the CMake preset's toolchainFile.

project(ffmpeg_tools VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8 /Zc:__cplusplus)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

find_package(Boost REQUIRED COMPONENTS program_options filesystem log)
find_package(OpenCV REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(indicators REQUIRED)
find_package(ffmpeg-nvenc REQUIRED)

# Platform-specific settings
if(WIN32)
    set(PLATFORM_DEFS _WIN32_WINNT=0x0A00)
    set(PLATFORM_LIBS "")
else()
    set(PLATFORM_DEFS "")
    set(PLATFORM_LIBS pthread)
endif()

# No ffmpeg binary is staged or shipped: the tools link the static libav*
# libraries from the ffmpeg-nvenc Conan package and run the codecs in-process.

# Shared library
add_library(common STATIC src/common.cpp src/common.h
    src/core/concepts.h
    src/core/scoped_handle.h
    src/core/format_util.h
    src/av/av_handles.h
    src/av/av_error.cpp src/av/av_error.h
    src/av/av_capabilities.cpp src/av/av_capabilities.h
    src/av/media_probe.cpp src/av/media_probe.h
    src/av/remuxer.cpp src/av/remuxer.h
    src/av/frame_grabber.cpp src/av/frame_grabber.h
    src/av/transcoder.cpp src/av/transcoder.h
    src/ffmpeg/progress_parser.h
    src/ffmpeg/progress_reporter.cpp src/ffmpeg/progress_reporter.h
    src/ffmpeg/ffmpeg.cpp src/ffmpeg/ffmpeg.h
    src/media/media_fs.cpp src/media/media_fs.h)
add_library(ffmpeg_tools::common ALIAS common)
target_compile_features(common PUBLIC cxx_std_20)
target_link_libraries(common PUBLIC Boost::program_options Boost::filesystem Boost::log indicators::indicators ffmpeg-nvenc::ffmpeg-nvenc ${PLATFORM_LIBS})
target_include_directories(common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(common PUBLIC ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(common)

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
