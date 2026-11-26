#pragma once

// CropDeduplicator -- decides whether an image crop is visually distinct from
// previously saved crops using resized normalized cross-correlation.

#include <initializer_list>
#include <span>

#include <opencv2/core.hpp>

class CropDeduplicator {
public:
    explicit CropDeduplicator(double threshold) noexcept;

    // True when crop is non-empty and visually distinct from every saved crop.
    [[nodiscard]] bool is_unique(const cv::Mat& crop,
                                 std::span<const cv::Mat> saved_crops) const;

    [[nodiscard]] bool is_unique(const cv::Mat& crop,
                                 std::initializer_list<cv::Mat> saved_crops) const {
        return is_unique(crop, std::span<const cv::Mat>{saved_crops.begin(), saved_crops.size()});
    }

private:
    double threshold_;
};
