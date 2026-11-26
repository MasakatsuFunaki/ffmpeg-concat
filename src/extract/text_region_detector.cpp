#include "extract/text_region_detector.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ranges>
#include <tuple>
#include <vector>

#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr int kChannelsGray = 1;
constexpr int kChannelsBgr = 3;
constexpr int kChannelsBgra = 4;

constexpr int kMserDelta = 5;
constexpr int kMserMinArea = 60;
constexpr int kMserMaxArea = 14400;
constexpr double kMserMaxVariation = 0.25;
constexpr double kMserMinDiversity = 0.2;

constexpr int kMinCharWidth = 5;
constexpr int kMinCharHeight = 8;
constexpr double kMaxCharRelativeSpan = 0.9;
constexpr double kMinCharAspect = 0.1;
constexpr double kMaxCharAspect = 10.0;
constexpr double kMaxCharRelArea = 0.5;
constexpr double kMinCharFillRatio = 0.1;
constexpr double kMaxCharFillRatio = 0.95;

constexpr int kVerticalGapDivisor = 2;
constexpr int kCenterDivisor = 2;

constexpr int kMinBlockWidth = 40;
constexpr int kMinBlockHeight = 12;
constexpr double kMinBlockAspect = 1.2;
constexpr double kMaxBlockRelArea = 0.4;
constexpr double kPadXFraction = 0.05;
constexpr double kPadYFraction = 0.1;

[[nodiscard]] cv::Mat to_grayscale(const cv::Mat& image) {
    if (image.channels() == kChannelsGray) {
        return image;
    }

    cv::Mat gray;
    if (image.channels() == kChannelsBgr) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
        return gray;
    }
    if (image.channels() == kChannelsBgra) {
        cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
        return gray;
    }
    return {};
}

}  // namespace

std::vector<cv::Rect> TextRegionDetector::detect(const cv::Mat& image) const {
    std::vector<cv::Rect> result;
    if (image.empty() || image.cols <= 0 || image.rows <= 0) {
        return result;
    }

    const cv::Mat gray = to_grayscale(image);
    if (gray.empty()) {
        return result;
    }

    auto mser = cv::MSER::create(kMserDelta, kMserMinArea, kMserMaxArea,
                                 kMserMaxVariation, kMserMinDiversity);

    std::vector<std::vector<cv::Point>> regions;
    std::vector<cv::Rect> bboxes;
    mser->detectRegions(gray, regions, bboxes);
    if (bboxes.empty()) {
        return result;
    }

    const double img_area =
        static_cast<double>(image.cols) * static_cast<double>(image.rows);
    const std::size_t region_count = std::min(bboxes.size(), regions.size());
    std::vector<cv::Rect> text_candidates;
    text_candidates.reserve(region_count);

    for (const std::size_t i : std::views::iota(std::size_t{0}, region_count)) {
        const cv::Rect& bb = bboxes[i];
        if (bb.width < kMinCharWidth || bb.height < kMinCharHeight) {
            continue;
        }
        if (static_cast<double>(bb.width) > static_cast<double>(image.cols) * kMaxCharRelativeSpan &&
            static_cast<double>(bb.height) > static_cast<double>(image.rows) * kMaxCharRelativeSpan) {
            continue;
        }

        const double aspect = static_cast<double>(bb.width) / static_cast<double>(bb.height);
        if (aspect < kMinCharAspect || aspect > kMaxCharAspect) {
            continue;
        }

        const double bb_area = static_cast<double>(bb.width) * static_cast<double>(bb.height);
        if (bb_area / img_area > kMaxCharRelArea) {
            continue;
        }

        const double fill_ratio = static_cast<double>(regions[i].size()) / bb_area;
        if (fill_ratio < kMinCharFillRatio || fill_ratio > kMaxCharFillRatio) {
            continue;
        }

        text_candidates.push_back(bb);
    }

    if (text_candidates.empty()) {
        return result;
    }

    std::ranges::sort(text_candidates, [](const cv::Rect& a, const cv::Rect& b) noexcept {
        return std::tie(a.y, a.x) < std::tie(b.y, b.x);
    });

    std::vector<cv::Rect> merged;
    std::vector<std::uint8_t> used(text_candidates.size(), 0U);

    for (const std::size_t i : std::views::iota(std::size_t{0}, text_candidates.size())) {
        if (used[i] != 0U) {
            continue;
        }

        cv::Rect block = text_candidates[i];
        used[i] = 1U;
        bool changed = true;

        while (changed) {
            changed = false;
            for (const std::size_t j : std::views::iota(std::size_t{0}, text_candidates.size())) {
                if (used[j] != 0U) {
                    continue;
                }

                const cv::Rect& cand = text_candidates[j];
                const int h_gap = std::max(block.height, cand.height);
                const int v_gap = std::max(block.height, cand.height) / kVerticalGapDivisor;

                const bool h_close = (cand.x <= block.x + block.width + h_gap) &&
                                     (block.x <= cand.x + cand.width + h_gap);
                const bool v_close =
                    std::abs((cand.y + cand.height / kCenterDivisor) -
                             (block.y + block.height / kCenterDivisor)) < v_gap;

                if (h_close && v_close) {
                    const int x1 = std::min(block.x, cand.x);
                    const int y1 = std::min(block.y, cand.y);
                    const int x2 = std::max(block.x + block.width, cand.x + cand.width);
                    const int y2 = std::max(block.y + block.height, cand.y + cand.height);
                    block = cv::Rect(x1, y1, x2 - x1, y2 - y1);
                    used[j] = 1U;
                    changed = true;
                }
            }
        }

        merged.push_back(block);
    }

    for (const auto& block : merged) {
        if (block.width < kMinBlockWidth || block.height < kMinBlockHeight) {
            continue;
        }

        const double aspect = static_cast<double>(block.width) / static_cast<double>(block.height);
        if (aspect < kMinBlockAspect) {
            continue;
        }

        const double block_area =
            static_cast<double>(block.width) * static_cast<double>(block.height);
        if (block_area / img_area > kMaxBlockRelArea) {
            continue;
        }

        const int pad_x = static_cast<int>(static_cast<double>(block.width) * kPadXFraction);
        const int pad_y = static_cast<int>(static_cast<double>(block.height) * kPadYFraction);
        const int x = std::max(0, block.x - pad_x);
        const int y = std::max(0, block.y - pad_y);
        const int w = std::min(block.width + pad_x + pad_x, image.cols - x);
        const int h = std::min(block.height + pad_y + pad_y, image.rows - y);
        result.emplace_back(x, y, w, h);
    }

    return result;
}
