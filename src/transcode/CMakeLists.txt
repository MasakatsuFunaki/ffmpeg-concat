add_executable(transcode transcode.cpp)
target_link_libraries(transcode PRIVATE encode av_transcode)
target_compile_definitions(transcode PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(transcode)
