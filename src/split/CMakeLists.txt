add_executable(split split.cpp)
target_link_libraries(split PRIVATE common Boost::program_options Boost::filesystem Boost::log ${PLATFORM_LIBS})
target_compile_definitions(split PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(split)
