add_test_target(test_system_st
    LIBS common ${TEST_OPENCV_LIBS}
    LABELS ST
)

add_dependencies(test_system_st
    concat_videos
    transcode
    cut
    split
    extract
)

set(ST_TOOL_PATHS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/tool_paths.hpp")

file(GENERATE
    OUTPUT "${ST_TOOL_PATHS_HEADER}"
    CONTENT [=[#pragma once

#include <string_view>

namespace test_support::tool_paths {

inline constexpr std::string_view kConcat = R"FFTOOLS($<TARGET_FILE:concat_videos>)FFTOOLS";
inline constexpr std::string_view kTranscode = R"FFTOOLS($<TARGET_FILE:transcode>)FFTOOLS";
inline constexpr std::string_view kCut = R"FFTOOLS($<TARGET_FILE:cut>)FFTOOLS";
inline constexpr std::string_view kSplit = R"FFTOOLS($<TARGET_FILE:split>)FFTOOLS";
inline constexpr std::string_view kExtract = R"FFTOOLS($<TARGET_FILE:extract>)FFTOOLS";

}  // namespace test_support::tool_paths
]=]
)

target_include_directories(test_system_st PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>"
)
