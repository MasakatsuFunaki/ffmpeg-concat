add_executable(cut cut.cpp)
target_link_libraries(cut PRIVATE common Boost::program_options Boost::filesystem Boost::log ${PLATFORM_LIBS})
target_compile_definitions(cut PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(cut)
