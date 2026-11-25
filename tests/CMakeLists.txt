find_package(GTest REQUIRED)

# Copy test data into build directory
file(COPY ${PROJECT_SOURCE_DIR}/tests/input DESTINATION ${CMAKE_BINARY_DIR}/tests)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/output)

# Path defines for tests (forward slashes for C++ string literals)
string(REPLACE "\\" "/" TEST_PROJECT_ROOT "${PROJECT_SOURCE_DIR}")
string(REPLACE "\\" "/" TEST_DATA_DIR "${CMAKE_BINARY_DIR}/tests/input")
string(REPLACE "\\" "/" TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tests/output")

# Test executables output directory
set(TEST_RUNTIME_DIR "${CMAKE_BINARY_DIR}/tests/bin")

# Static libraries for testing (compile app sources without main)
add_library(concat_lib STATIC ${PROJECT_SOURCE_DIR}/src/concat/concat.cpp)
target_link_libraries(concat_lib PUBLIC common Boost::program_options Boost::filesystem)
target_compile_definitions(concat_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)

add_library(transcode_lib STATIC ${PROJECT_SOURCE_DIR}/src/transcode/transcode.cpp)
target_link_libraries(transcode_lib PUBLIC encode common Boost::program_options Boost::filesystem)
target_compile_definitions(transcode_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)

add_library(cut_lib STATIC ${PROJECT_SOURCE_DIR}/src/cut/cut.cpp)
target_link_libraries(cut_lib PUBLIC common Boost::program_options Boost::filesystem)
target_compile_definitions(cut_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)

add_library(split_lib STATIC ${PROJECT_SOURCE_DIR}/src/split/split.cpp)
target_link_libraries(split_lib PUBLIC common Boost::program_options Boost::filesystem)
target_compile_definitions(split_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)

if(TARGET opencv_core)
    set(EXTRACT_OPENCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs opencv_features2d)
else()
    set(EXTRACT_OPENCV_LIBS ${OpenCV_LIBS})
endif()

add_library(extract_lib STATIC ${PROJECT_SOURCE_DIR}/src/extract/extract.cpp)
target_link_libraries(extract_lib PUBLIC extractcore common Boost::program_options Boost::filesystem ${EXTRACT_OPENCV_LIBS} OpenSSL::Crypto)
target_compile_definitions(extract_lib PRIVATE ${PLATFORM_DEFS} TESTING_NO_MAIN)

# Helper function to create a test target with standard configuration
function(add_test_target TARGET_NAME)
    set(multiValueArgs LIBS LABELS)
    cmake_parse_arguments(ARG "" "" "${multiValueArgs}" ${ARGN})

    add_executable(${TARGET_NAME} ${TARGET_NAME}.cpp)
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_RUNTIME_DIR}"
    )
    target_link_libraries(${TARGET_NAME} PRIVATE ${ARG_LIBS} GTest::gtest_main GTest::gtest ${PLATFORM_LIBS})
    target_compile_definitions(${TARGET_NAME} PRIVATE
        ${PLATFORM_DEFS}
        PROJECT_SOURCE_DIR="${TEST_PROJECT_ROOT}"
        TEST_DATA_DIR="${TEST_DATA_DIR}"
        TEST_OUTPUT_DIR="${TEST_OUTPUT_DIR}"
    )
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
