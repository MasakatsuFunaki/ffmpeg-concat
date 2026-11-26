#pragma once

// FrameScorer -- rates a single frame's visual interestingness from sharpness,
// HSV saturation, grayscale contrast, and Canny edge density.
// Thread-safe: stateless and const-only.

#include <filesystem>

#include <opencv2/core.hpp>

namespace fs = std::filesystem;

class FrameScorer {
public:
    [[nodiscard]] double score(const fs::path& image_path) const;
    [[nodiscard]] double score(const cv::Mat& frame) const;
};
