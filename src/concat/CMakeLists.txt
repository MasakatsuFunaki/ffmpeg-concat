add_executable(concat_videos concat.cpp)
target_link_libraries(concat_videos PRIVATE common Boost::program_options Boost::filesystem Boost::log ${PLATFORM_LIBS})
target_compile_definitions(concat_videos PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(concat_videos)
