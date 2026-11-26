add_executable(cut cut.cpp)
target_link_libraries(cut PRIVATE av_remux av_transcode)
target_compile_definitions(cut PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(cut)
