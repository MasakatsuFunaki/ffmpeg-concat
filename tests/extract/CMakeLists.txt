add_test_target(test_extract LIBS extract_lib LABELS CT)
add_test_target(test_extract_core_ut LIBS extractcore common ${TEST_OPENCV_LIBS} OpenSSL::Crypto LABELS UT)
