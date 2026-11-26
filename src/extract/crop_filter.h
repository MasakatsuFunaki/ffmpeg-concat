#pragma once

// CropFilter -- reject-and-stop Chain of Responsibility protocol over
// single-channel grayscale candidate crops.

#include <opencv2/core.hpp>

class CropFilter {
public:
    virtual ~CropFilter();

    CropFilter(const CropFilter&) = delete;
    CropFilter& operator=(const CropFilter&) = delete;

    [[nodiscard]] virtual bool accept(const cv::Mat& crop_gray) const = 0;

protected:
    CropFilter() = default;
    CropFilter(CropFilter&&) noexcept = default;
    CropFilter& operator=(CropFilter&&) noexcept = default;
};

// Rejects near-uniform patches with grayscale standard deviation below 25.0.
class ContrastFilter final : public CropFilter {
public:
    [[nodiscard]] bool accept(const cv::Mat& crop_gray) const override;
};

// Rejects crops whose Canny edge pixel fraction is below 0.05.
class EdgeDensityFilter final : public CropFilter {
public:
    [[nodiscard]] bool accept(const cv::Mat& crop_gray) const override;
};

// Rejects crops with fewer than 1.0 Otsu connected component per 100 px width.
class ComponentCountFilter final : public CropFilter {
public:
    [[nodiscard]] bool accept(const cv::Mat& crop_gray) const override;
};
