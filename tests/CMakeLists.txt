find_package(GTest REQUIRED)

# Copy committed read-only test fixtures into build directory
file(COPY ${PROJECT_SOURCE_DIR}/tests/input DESTINATION ${CMAKE_BINARY_DIR}/tests)

# Path define for read-only fixtures (forward slashes for C++ string literals)
string(REPLACE "\\" "/" TEST_DATA_DIR "${CMAKE_BINARY_DIR}/tests/input")

# Test executables output directory
set(TEST_RUNTIME_DIR "${CMAKE_BINARY_DIR}/tests/bin")

if(TARGET opencv_core)
    set(TEST_OPENCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs opencv_features2d)
else()
    set(TEST_OPENCV_LIBS ${OpenCV_LIBS})
endif()

# Static libraries for component testing (compile app sources without main)
add_library(concat_lib STATIC ${PROJECT_SOURCE_DIR}/src/concat/concat.cpp)
target_link_libraries(concat_lib PUBLIC common Boost::program_options Boost::filesystem)
target_compile_definitions(concat_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)
ffmpeg_tools_set_warnings(concat_lib)

add_library(transcode_lib STATIC ${PROJECT_SOURCE_DIR}/src/transcode/transcode.cpp)
target_link_libraries(transcode_lib PUBLIC encode common Boost::program_options Boost::filesystem)
target_compile_definitions(transcode_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)
ffmpeg_tools_set_warnings(transcode_lib)

add_library(cut_lib STATIC ${PROJECT_SOURCE_DIR}/src/cut/cut.cpp)
target_link_libraries(cut_lib PUBLIC common Boost::program_options Boost::filesystem)
target_compile_definitions(cut_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)
ffmpeg_tools_set_warnings(cut_lib)

add_library(split_lib STATIC ${PROJECT_SOURCE_DIR}/src/split/split.cpp)
target_link_libraries(split_lib PUBLIC common Boost::program_options Boost::filesystem)
target_compile_definitions(split_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)
ffmpeg_tools_set_warnings(split_lib)

add_library(extract_lib STATIC ${PROJECT_SOURCE_DIR}/src/extract/extract.cpp)
target_link_libraries(extract_lib PUBLIC extractcore common Boost::program_options Boost::filesystem ${TEST_OPENCV_LIBS} OpenSSL::Crypto)
target_compile_definitions(extract_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)
ffmpeg_tools_set_warnings(extract_lib)

# Helper function to create a test target with standard configuration and warnings
function(add_test_target TARGET_NAME)
    set(multiValueArgs LIBS LABELS)
    cmake_parse_arguments(ARG "" "" "${multiValueArgs}" ${ARGN})

    add_executable(${TARGET_NAME} ${TARGET_NAME}.cpp)
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_RUNTIME_DIR}"
    )
    target_include_directories(${TARGET_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(${TARGET_NAME} PRIVATE ${ARG_LIBS} GTest::gtest_main GTest::gtest ${PLATFORM_LIBS})
    target_compile_definitions(${TARGET_NAME} PRIVATE
        ${PLATFORM_DEFS}
        TEST_DATA_DIR="${TEST_DATA_DIR}"
    )
    ffmpeg_tools_set_warnings(${TARGET_NAME})
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    if(ARG_LABELS)
        set_tests_properties(${TARGET_NAME} PROPERTIES LABELS "${ARG_LABELS}")
    endif()
endfunction()

add_subdirectory(common)
add_subdirectory(encode)
add_subdirectory(concat)
add_subdirectory(transcode)
add_subdirectory(cut)
add_subdirectory(split)
add_subdirectory(extract)
add_subdirectory(system)
