add_executable(split split.cpp)
target_link_libraries(split PRIVATE av_remux)
target_compile_definitions(split PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(split)
