#include "extract/text_region_detector.h"

#include <algorithm>
#include <cstdlib>

#include <opencv2/imgproc.hpp>
#include <opencv2/features2d.hpp>

std::vector<cv::Rect> TextRegionDetector::detect(const cv::Mat& image) const {
    std::vector<cv::Rect> result;
    if (image.empty()) return result;

    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);

    // Use MSER to detect text-like stable regions
    auto mser = cv::MSER::create(
        5,      // delta
        60,     // min area
        14400,  // max area
        0.25,   // max variation
        0.2     // min diversity
    );

    std::vector<std::vector<cv::Point>> regions;
    std::vector<cv::Rect> bboxes;
    mser->detectRegions(gray, regions, bboxes);

    if (bboxes.empty()) return result;

    // Filter MSER regions for text-like characteristics:
    // - Aspect ratio: text regions are wider than tall (or at least not extremely tall/narrow)
    // - Size: not too small, not too large relative to image
    // - Stroke width consistency (approximated by area vs bounding box area ratio)
    std::vector<cv::Rect> text_candidates;
    double img_area = static_cast<double>(image.cols * image.rows);

    for (size_t i = 0; i < bboxes.size(); ++i) {
        const cv::Rect& bb = bboxes[i];
        if (bb.width < 5 || bb.height < 8) continue;
        if (bb.width > image.cols * 0.9 && bb.height > image.rows * 0.9) continue;

        double aspect = static_cast<double>(bb.width) / bb.height;
        // Individual characters have aspect ratios roughly 0.2 to 2.0
        if (aspect < 0.1 || aspect > 10.0) continue;

        // Area relative to image - skip very large regions
        double rel_area = (bb.width * bb.height) / img_area;
        if (rel_area > 0.5) continue;

        // Fill ratio: text characters have moderate fill
        double fill_ratio = static_cast<double>(regions[i].size()) / (bb.width * bb.height);
        if (fill_ratio < 0.1 || fill_ratio > 0.95) continue;

        text_candidates.push_back(bb);
    }

    if (text_candidates.empty()) return result;

    // Group nearby text candidates into text blocks/lines using spatial clustering.
    // Sort by y-coordinate first, then group by proximity.
    std::sort(text_candidates.begin(), text_candidates.end(),
              [](const cv::Rect& a, const cv::Rect& b) {
                  return (a.y < b.y) || (a.y == b.y && a.x < b.x);
              });

    // Merge overlapping/nearby rectangles into text blocks
    // Use a simple approach: iteratively merge rects that are close enough
    std::vector<cv::Rect> merged;
    std::vector<bool> used(text_candidates.size(), false);

    for (size_t i = 0; i < text_candidates.size(); ++i) {
        if (used[i]) continue;

        cv::Rect block = text_candidates[i];
        used[i] = true;
        bool changed = true;

        while (changed) {
            changed = false;
            for (size_t j = 0; j < text_candidates.size(); ++j) {
                if (used[j]) continue;

                const cv::Rect& cand = text_candidates[j];

                // Check if candidate is close to current block
                // Horizontal gap threshold: based on typical character spacing
                int h_gap = std::max(block.height, cand.height);
                int v_gap = std::max(block.height, cand.height) / 2;

                bool h_close = (cand.x <= block.x + block.width + h_gap) &&
                               (block.x <= cand.x + cand.width + h_gap);
                bool v_close = std::abs((cand.y + cand.height / 2) -
                                        (block.y + block.height / 2)) < v_gap;

                if (h_close && v_close) {
                    // Merge
                    int x1 = std::min(block.x, cand.x);
                    int y1 = std::min(block.y, cand.y);
                    int x2 = std::max(block.x + block.width, cand.x + cand.width);
                    int y2 = std::max(block.y + block.height, cand.y + cand.height);
                    block = cv::Rect(x1, y1, x2 - x1, y2 - y1);
                    used[j] = true;
                    changed = true;
                }
            }
        }

        merged.push_back(block);
    }

    // Final filtering: keep only blocks that look like text lines/paragraphs
    for (const auto& block : merged) {
        double aspect = static_cast<double>(block.width) / block.height;

        // Text blocks should be wider than tall (aspect > 1.5) or have minimum size
        // Also accept shorter text blocks that have enough width
        if (block.width < 40 || block.height < 12) continue;

        // Text should be reasonably wide compared to its height
        // Single words: aspect ~2-10, text lines: aspect ~5-50
        // Allow some narrow blocks (subtitles can be aspect ~3)
        if (aspect < 1.2) continue;

        // Skip blocks that are too large (probably not text)
        double rel_area = static_cast<double>(block.width * block.height) / img_area;
        if (rel_area > 0.4) continue;

        // Add padding around the text block (5% of dimensions)
        int pad_x = static_cast<int>(block.width * 0.05);
        int pad_y = static_cast<int>(block.height * 0.1);
        cv::Rect padded(
            std::max(0, block.x - pad_x),
            std::max(0, block.y - pad_y),
            std::min(block.width + 2 * pad_x, image.cols - std::max(0, block.x - pad_x)),
            std::min(block.height + 2 * pad_y, image.rows - std::max(0, block.y - pad_y))
        );

        result.push_back(padded);
    }

    return result;
}
