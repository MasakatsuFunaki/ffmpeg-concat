add_executable(transcode transcode.cpp)
target_link_libraries(transcode PRIVATE encode common Boost::program_options Boost::filesystem Boost::log ${PLATFORM_LIBS})
target_compile_definitions(transcode PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(transcode)
