#pragma once

// ---------------------------------------------------------------------------
// FrameScorer – plain class (deliberately NOT a pattern) that rates a single
// frame's visual interestingness by combining sharpness, colour saturation,
// contrast and edge density. Used by the striking-frame extractor to rank
// candidates.
// ---------------------------------------------------------------------------

#include <filesystem>

namespace fs = std::filesystem;

class FrameScorer {
public:
    // Compute a visual interestingness score for an image file.
    double score(const fs::path& image_path) const;
};
