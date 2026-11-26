#include "extract/mosaic_builder.h"

#include "extract/fingerprint_cache.h"
#include "extract/image_io.h"
#include "core/path_util.h"
#include "core/profiler.h"
#include "core/thread_pool.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr int kMinMosaicCount = 1;
constexpr int kMinMosaicStart = 1;
constexpr int kParallelDecodeThreshold = 4;
constexpr int kCanvasWidth = 1920;
constexpr int kCanvasHeight = 1080;
constexpr std::string_view kStrikingPrefix = "striking_";
constexpr std::string_view kJpgExtension = ".jpg";

}  // namespace

bool MosaicBuilder::build(const fs::path& detect_dir,
                          const fs::path& mosaic_dir,
                          int mosaic_count,
                          int mosaic_start) const {
    core::ScopedProfile prof("extract.mosaic.build");

    std::error_code ec;
    if (mosaic_count < kMinMosaicCount || mosaic_start < kMinMosaicStart ||
        !fs::exists(detect_dir, ec) || !fs::is_directory(detect_dir, ec)) {
        return false;
    }

    const fs::path mosaic_file = mosaic_dir / "mosaic.jpg";
    const fs::path hash_file = mosaic_dir / ".mosaic_hash";

    const std::string detect_fp =
        FingerprintCache::compute_dir_fingerprint(detect_dir, kStrikingPrefix);
    const std::string current_fp =
        std::format("n={},p={}\n{}", mosaic_count, mosaic_start, detect_fp);

    if (fs::exists(mosaic_file, ec)) {
        const std::string stored_fp = FingerprintCache::load(hash_file);
        if (!stored_fp.empty() && stored_fp == current_fp) {
            BOOST_LOG_TRIVIAL(info)
                << "  Mosaic cache hit — inputs unchanged, skipping regeneration";
            return true;
        }
        BOOST_LOG_TRIVIAL(info) << "  Mosaic inputs changed, regenerating...";
    }

    fs::directory_iterator dir_it(detect_dir, ec);
    if (ec) {
        return false;
    }

    std::vector<fs::path> striking_files;
    for (const auto& entry : dir_it) {
        if (entry.is_regular_file() &&
            entry.path().extension() == kJpgExtension &&
            core::to_utf8(entry.path().filename()).starts_with(kStrikingPrefix)) {
            striking_files.push_back(entry.path());
        }
    }
    std::ranges::sort(striking_files);

    if (striking_files.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "  No striking frames found for mosaic";
        return false;
    }

    const int total = static_cast<int>(striking_files.size());
    if (mosaic_start > total) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "  Error: --mosaic start position P={} is out of range (only {} striking frame(s) available).",
            mosaic_start, total);
        return false;
    }

    const auto subset = std::span<const fs::path>{striking_files}.subspan(
        static_cast<std::size_t>(mosaic_start - 1));
    const int available = static_cast<int>(subset.size());

    if (available < mosaic_count) {
        BOOST_LOG_TRIVIAL(error) << std::format(
            "  Error: Not enough striking frames from position P={} for mosaic of N={} (only {} frame(s) available from that position).",
            mosaic_start, mosaic_count, available);
        return false;
    }

    std::vector<fs::path> selected_paths;
    selected_paths.reserve(static_cast<std::size_t>(mosaic_count));
    for (const int i : std::views::iota(0, mosaic_count)) {
        int idx = (mosaic_count == 1)
                      ? 0
                      : static_cast<int>(std::round(
                            static_cast<double>(i) * (available - 1) / (mosaic_count - 1)));
        idx = std::min(idx, available - 1);
        selected_paths.push_back(subset[static_cast<std::size_t>(idx)]);
    }

    std::vector<cv::Mat> decoded(selected_paths.size());
    if (mosaic_count >= kParallelDecodeThreshold) {
        const std::size_t worker_count =
            std::min(selected_paths.size(), core::ThreadPool::default_thread_count());
        core::ThreadPool pool(worker_count);
        pool.parallel_for(selected_paths.size(), [&selected_paths, &decoded](std::size_t i) {
            decoded[i] = extract_io::read_image(selected_paths[i]);
        });
    } else {
        for (const std::size_t i : std::views::iota(std::size_t{0}, selected_paths.size())) {
            decoded[i] = extract_io::read_image(selected_paths[i]);
        }
    }

    std::vector<cv::Mat> images;
    images.reserve(decoded.size());
    for (auto& img : decoded) {
        if (!img.empty()) {
            images.push_back(std::move(img));
        }
    }

    if (images.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "  No images could be loaded for mosaic";
        return false;
    }

    cv::Mat canvas(kCanvasHeight, kCanvasWidth, CV_8UC3, cv::Scalar(0, 0, 0));

    const int actual_n = static_cast<int>(images.size());
    const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(actual_n))));
    const int rows = (actual_n + cols - 1) / cols;
    const int cell_h = kCanvasHeight / rows;

    std::size_t img_idx = 0;
    for (const int r : std::views::iota(0, rows)) {
        const int items_in_row = std::min(cols, actual_n - r * cols);
        const int cell_w = kCanvasWidth / items_in_row;
        const int y = r * cell_h;
        const int h = (r == rows - 1) ? (kCanvasHeight - y) : cell_h;

        for (const int c : std::views::iota(0, items_in_row)) {
            const int x = c * cell_w;
            const int w = (c == items_in_row - 1) ? (kCanvasWidth - x) : cell_w;

            cv::Mat cell;
            cv::resize(images[img_idx], cell, cv::Size(w, h));
            cell.copyTo(canvas(cv::Rect(x, y, w, h)));
            ++img_idx;
        }
    }

    fs::create_directories(mosaic_dir);
    const std::string output_utf8 = core::to_utf8(mosaic_file);
    if (!extract_io::write_jpeg(mosaic_file, canvas)) {
        BOOST_LOG_TRIVIAL(warning) << "  Failed to write mosaic to " << output_utf8;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << std::format(
        "  Created mosaic ({} images) at {}", actual_n, output_utf8);
    FingerprintCache::save(hash_file, current_fp);
    return true;
}
