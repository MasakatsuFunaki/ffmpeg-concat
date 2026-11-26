#pragma once

// CropDeduplicator -- decides whether an image crop is visually distinct from
// previously saved crops using resized normalized cross-correlation.
// Thread-compatible: const methods are safe for concurrent reads when no
// mutating method (try_add / clear_cache) runs concurrently.

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>

#include <opencv2/core.hpp>

class CropDeduplicator {
public:
    explicit CropDeduplicator(double threshold) noexcept;
    ~CropDeduplicator();

    CropDeduplicator(const CropDeduplicator&) = delete;
    CropDeduplicator& operator=(const CropDeduplicator&) = delete;
    CropDeduplicator(CropDeduplicator&&) noexcept;
    CropDeduplicator& operator=(CropDeduplicator&&) noexcept;

    // Adds crop's precomputed thumbnail to the internal cache if crop is valid
    // and visually distinct (< threshold) from every cached thumbnail.
    [[nodiscard]] bool try_add(const cv::Mat& crop);

    // Clears all cached thumbnails without altering the similarity threshold.
    void clear_cache() noexcept;

    // True when crop is non-empty and visually distinct from every saved crop.
    [[nodiscard]] bool is_unique(const cv::Mat& crop,
                                 std::span<const cv::Mat> saved_crops) const;

    [[nodiscard]] bool is_unique(const cv::Mat& crop,
                                 std::initializer_list<cv::Mat> saved_crops) const {
        return is_unique(crop, std::span<const cv::Mat>{saved_crops.begin(), saved_crops.size()});
    }

    // Same visual distinctness check as try_add(), without mutating the cache.
    [[nodiscard]] bool is_unique_against_cached(const cv::Mat& crop) const;

    [[nodiscard]] std::size_t cached_count() const noexcept;

private:
    struct Impl;

    double threshold_;
    std::unique_ptr<Impl> impl_;
};
