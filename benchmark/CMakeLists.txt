# benchmark/CMakeLists.txt -- Google Benchmark suite and profiler/thread-pool UTs.

set(BENCHMARK_RUNTIME_DIR "${CMAKE_BINARY_DIR}/benchmark/bin")
set(BENCHMARK_TEST_RUNTIME_DIR "${CMAKE_BINARY_DIR}/tests/bin")

if(BUILD_TESTING OR BUILD_BENCHMARKS)
    find_package(GTest REQUIRED)

    add_executable(test_profiler_ut test_profiler_ut.cpp)
    set_target_properties(test_profiler_ut PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${BENCHMARK_TEST_RUNTIME_DIR}"
    )
    target_link_libraries(test_profiler_ut PRIVATE
        common
        GTest::gtest_main
        GTest::gtest
        ${PLATFORM_LIBS}
    )
    target_compile_definitions(test_profiler_ut PRIVATE ${PLATFORM_DEFS})
    ffmpeg_tools_set_warnings(test_profiler_ut)

    if(BUILD_TESTING)
        add_test(NAME test_profiler_ut COMMAND test_profiler_ut)
        set_tests_properties(test_profiler_ut PROPERTIES LABELS "UT")
    endif()
endif()

if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED)

    if(TARGET opencv_core)
        set(BENCH_OPENCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs opencv_features2d)
    else()
        set(BENCH_OPENCV_LIBS ${OpenCV_LIBS})
    endif()

    string(REPLACE "\\" "/" BENCHMARK_DATA_DIR "${PROJECT_SOURCE_DIR}/tests/input")

    add_executable(ffmpeg_tools_bench bench_main.cpp)
    set_target_properties(ffmpeg_tools_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${BENCHMARK_RUNTIME_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BENCHMARK_RUNTIME_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${BENCHMARK_RUNTIME_DIR}"
    )
    target_link_libraries(ffmpeg_tools_bench PRIVATE
        extractcore
        common
        ${BENCH_OPENCV_LIBS}
        OpenSSL::Crypto
        benchmark::benchmark
        ${PLATFORM_LIBS}
    )
    target_compile_definitions(ffmpeg_tools_bench PRIVATE
        ${PLATFORM_DEFS}
        BENCHMARK_DATA_DIR="${BENCHMARK_DATA_DIR}"
    )
    ffmpeg_tools_set_warnings(ffmpeg_tools_bench)
endif()
