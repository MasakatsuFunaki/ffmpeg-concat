#pragma once

// ---------------------------------------------------------------------------
// CropDeduplicator – plain class (deliberately NOT a pattern) that decides
// whether a crop is visually distinct from a set of already-saved crops via a
// resized normalized-correlation comparison. The similarity threshold is fixed
// at construction (0.85 for striking frames, 0.92 for text crops).
// ---------------------------------------------------------------------------

#include <vector>

#include <opencv2/core.hpp>

class CropDeduplicator {
public:
    explicit CropDeduplicator(double threshold);

    // True when crop is visually different from all previously saved crops.
    bool is_unique(const cv::Mat& crop, const std::vector<cv::Mat>& saved_crops) const;

private:
    double threshold_;
};
