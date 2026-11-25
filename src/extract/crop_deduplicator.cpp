#include "extract/crop_deduplicator.h"

#include <opencv2/imgproc.hpp>

CropDeduplicator::CropDeduplicator(double threshold) : threshold_(threshold) {}

bool CropDeduplicator::is_unique(const cv::Mat& crop,
                                 const std::vector<cv::Mat>& saved_crops) const {
    if (saved_crops.empty()) return true;

    // Resize crop to a standard size for comparison
    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(128, 64));
    cv::Mat gray_new;
    cv::cvtColor(resized, gray_new, cv::COLOR_BGR2GRAY);

    for (const auto& saved : saved_crops) {
        cv::Mat saved_resized;
        cv::resize(saved, saved_resized, cv::Size(128, 64));
        cv::Mat gray_saved;
        cv::cvtColor(saved_resized, gray_saved, cv::COLOR_BGR2GRAY);

        // Compare using normalized correlation
        cv::Mat result;
        cv::matchTemplate(gray_new, gray_saved, result, cv::TM_CCOEFF_NORMED);
        double similarity = result.at<float>(0, 0);

        if (similarity >= threshold_)
            return false;  // Too similar to an existing crop
    }

    return true;
}
