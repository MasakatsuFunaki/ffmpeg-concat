# extractcore — pattern-component library backing the extract tool. Holds all
# the OpenCV/OpenSSL heavy lifting (frame-extraction strategies, the crop-filter
# chain, fingerprint cache and mosaic builder) so extract.cpp stays a thin CLI
# shell and the test's extract_lib links the same components.
if(TARGET opencv_core)
    set(EXTRACT_OPENCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs opencv_features2d)
else()
    set(EXTRACT_OPENCV_LIBS ${OpenCV_LIBS})
endif()

add_library(extractcore STATIC
    image_io.h
    frame_extractor.cpp frame_extractor.h
    equally_spaced_extractor.cpp equally_spaced_extractor.h
    striking_frame_extractor.cpp striking_frame_extractor.h
    text_crop_extractor.cpp text_crop_extractor.h
    frame_scorer.cpp frame_scorer.h
    text_region_detector.cpp text_region_detector.h
    crop_filter.cpp crop_filter.h
    crop_deduplicator.cpp crop_deduplicator.h
    mosaic_builder.cpp mosaic_builder.h
    fingerprint_cache.cpp fingerprint_cache.h)
add_library(ffmpeg_tools::extractcore ALIAS extractcore)
target_link_libraries(extractcore PUBLIC av_frame ${EXTRACT_OPENCV_LIBS} OpenSSL::Crypto)
target_compile_definitions(extractcore PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(extractcore)

add_executable(extract extract.cpp)
target_link_libraries(extract PRIVATE extractcore av_frame Boost::program_options Boost::log ${EXTRACT_OPENCV_LIBS} OpenSSL::Crypto ${PLATFORM_LIBS})
target_compile_definitions(extract PRIVATE ${PLATFORM_DEFS})
ffmpeg_tools_set_warnings(extract)
