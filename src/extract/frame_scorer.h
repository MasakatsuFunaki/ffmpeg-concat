#pragma once

// FrameScorer -- rates a single frame's visual interestingness from sharpness,
// HSV saturation, grayscale contrast, and Canny edge density.

#include <filesystem>

namespace fs = std::filesystem;

class FrameScorer {
public:
    [[nodiscard]] double score(const fs::path& image_path) const;
};
