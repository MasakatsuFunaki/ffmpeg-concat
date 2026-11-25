add_library(encode STATIC encoder_profile.cpp encoder_profile.h encoder_catalog.cpp encoder_catalog.h
    encode_plan.cpp encode_plan.h)
add_library(ffmpeg_tools::encode ALIAS encode)
target_link_libraries(encode PUBLIC common)
target_compile_definitions(encode PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(encode)
