#include "extract/text_region_detector.h"

#include "core/profiler.h"

#include <algorithm>
#include <cmath>
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

constexpr int kMaxDetectWidth = 1280;
constexpr int kMinScaledMserArea = 10;
constexpr int kMinScaledCharWidth = 3;
constexpr int kMinScaledCharHeight = 5;
constexpr int kMinScaledBlockWidth = 20;
constexpr int kMinScaledBlockHeight = 8;

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
    core::ScopedProfile prof("extract.text.mser_detect");

    std::vector<cv::Rect> result;
    if (image.empty() || image.cols <= 0 || image.rows <= 0) {
        return result;
    }

    const cv::Mat gray = to_grayscale(image);
    if (gray.empty()) {
        return result;
    }

    // Downscale wide HD/4K frames to 1280px width for MSER detection (2.25x+ fewer
    // pixels) and scale geometric thresholds and detected bounding boxes proportionally.
    cv::Mat work_gray = gray;
    double scale = 1.0;
    if (gray.cols > kMaxDetectWidth) {
        scale = static_cast<double>(kMaxDetectWidth) / static_cast<double>(gray.cols);
        const int scaled_h =
            std::max(1, static_cast<int>(std::lround(static_cast<double>(gray.rows) * scale)));
        cv::resize(gray, work_gray, cv::Size(kMaxDetectWidth, scaled_h), 0.0, 0.0, cv::INTER_AREA);
    }

    const double area_scale = scale * scale;
    const int min_area = (scale < 1.0)
                             ? std::max(kMinScaledMserArea,
                                        static_cast<int>(std::lround(
                                            static_cast<double>(kMserMinArea) * area_scale)))
                             : kMserMinArea;
    const int max_area = (scale < 1.0)
                             ? std::max(min_area + 1,
                                        static_cast<int>(std::lround(
                                            static_cast<double>(kMserMaxArea) * area_scale)))
                             : kMserMaxArea;
    const int min_char_w = (scale < 1.0)
                               ? std::max(kMinScaledCharWidth,
                                          static_cast<int>(std::lround(
                                              static_cast<double>(kMinCharWidth) * scale)))
                               : kMinCharWidth;
    const int min_char_h = (scale < 1.0)
                               ? std::max(kMinScaledCharHeight,
                                          static_cast<int>(std::lround(
                                              static_cast<double>(kMinCharHeight) * scale)))
                               : kMinCharHeight;
    const int min_block_w = (scale < 1.0)
                                ? std::max(kMinScaledBlockWidth,
                                           static_cast<int>(std::lround(
                                               static_cast<double>(kMinBlockWidth) * scale)))
                                : kMinBlockWidth;
    const int min_block_h = (scale < 1.0)
                                ? std::max(kMinScaledBlockHeight,
                                           static_cast<int>(std::lround(
                                               static_cast<double>(kMinBlockHeight) * scale)))
                                : kMinBlockHeight;

    auto mser = cv::MSER::create(kMserDelta, min_area, max_area,
                                 kMserMaxVariation, kMserMinDiversity);

    std::vector<std::vector<cv::Point>> regions;
    std::vector<cv::Rect> bboxes;
    mser->detectRegions(work_gray, regions, bboxes);
    if (bboxes.empty()) {
        return result;
    }

    const double img_area =
        static_cast<double>(work_gray.cols) * static_cast<double>(work_gray.rows);
    const std::size_t region_count = std::min(bboxes.size(), regions.size());
    std::vector<cv::Rect> text_candidates;
    text_candidates.reserve(region_count);

    for (const std::size_t i : std::views::iota(std::size_t{0}, region_count)) {
        const cv::Rect& bb = bboxes[i];
        if (bb.width < min_char_w || bb.height < min_char_h) {
            continue;
        }
        if (static_cast<double>(bb.width) > static_cast<double>(work_gray.cols) * kMaxCharRelativeSpan &&
            static_cast<double>(bb.height) > static_cast<double>(work_gray.rows) * kMaxCharRelativeSpan) {
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

    int max_cand_height = 0;
    for (const auto& cand : text_candidates) {
        max_cand_height = std::max(max_cand_height, cand.height);
    }

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
            int max_y_limit = block.y + block.height + max_cand_height;
            for (const std::size_t j : std::views::iota(i + 1, text_candidates.size())) {
                const cv::Rect& cand = text_candidates[j];
                if (cand.y > max_y_limit) {
                    break;
                }
                if (used[j] != 0U) {
                    continue;
                }

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
                    max_y_limit = block.y + block.height + max_cand_height;
                    used[j] = 1U;
                    changed = true;
                }
            }
        }

        merged.push_back(block);
    }

    const double inv_x = static_cast<double>(image.cols) / static_cast<double>(work_gray.cols);
    const double inv_y = static_cast<double>(image.rows) / static_cast<double>(work_gray.rows);

    for (const auto& block : merged) {
        if (block.width < min_block_w || block.height < min_block_h) {
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

        const int full_x = (scale < 1.0)
                               ? static_cast<int>(std::floor(static_cast<double>(block.x) * inv_x))
                               : block.x;
        const int full_y = (scale < 1.0)
                               ? static_cast<int>(std::floor(static_cast<double>(block.y) * inv_y))
                               : block.y;
        const int full_w = (scale < 1.0)
                               ? static_cast<int>(std::ceil(static_cast<double>(block.width) * inv_x))
                               : block.width;
        const int full_h = (scale < 1.0)
                               ? static_cast<int>(std::ceil(static_cast<double>(block.height) * inv_y))
                               : block.height;

        const int pad_x = static_cast<int>(static_cast<double>(full_w) * kPadXFraction);
        const int pad_y = static_cast<int>(static_cast<double>(full_h) * kPadYFraction);
        const int x = std::max(0, full_x - pad_x);
        const int y = std::max(0, full_y - pad_y);
        const int w = std::min(full_w + pad_x + pad_x, image.cols - x);
        const int h = std::min(full_h + pad_y + pad_y, image.rows - y);
        result.emplace_back(x, y, w, h);
    }

    return result;
}
