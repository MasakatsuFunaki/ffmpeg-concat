#pragma once

// ---------------------------------------------------------------------------
// CropFilter – Chain of Responsibility (GoF), reject-and-stop. Each concrete
// filter computes its own metric from the grayscale crop and votes accept or
// reject. The text extractor runs them in order Contrast -> EdgeDensity ->
// ComponentCount and drops the crop on the first rejection, so the original
// short-circuit evaluation is preserved exactly (later filters never run when
// an earlier one rejects).
// ---------------------------------------------------------------------------

#include <opencv2/core.hpp>

class CropFilter {
public:
    virtual ~CropFilter() = default;
    virtual bool accept(const cv::Mat& crop_gray) const = 0;
};

// Reject near-uniform patches (plain backgrounds): reject stddev < 25.0.
class ContrastFilter : public CropFilter {
public:
    bool accept(const cv::Mat& crop_gray) const override;
};

// Reject crops with too few edge pixels for text strokes: reject ratio < 0.05.
class EdgeDensityFilter : public CropFilter {
public:
    bool accept(const cv::Mat& crop_gray) const override;
};

// Reject crops with too few text-like connected components: reject
// (num_components - 1) * 100 / cols < 1.0.
class ComponentCountFilter : public CropFilter {
public:
    bool accept(const cv::Mat& crop_gray) const override;
};
