#include "extract/equally_spaced_extractor.h"

#include "extract/image_io.h"
#include "av/frame_sampler.h"
#include "core/format_util.h"
#include "core/path_util.h"
#include "core/profiler.h"
#include "core/thread_pool.h"
#include "ffmpeg/ffmpeg.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>

namespace {

constexpr double kIntervalCenterOffset = 0.5;
constexpr double kEndClampMarginSec = 0.01;
constexpr int kFrameIndexWidth = 4;
constexpr std::size_t kMinWriteBatchSize = 32;
constexpr std::size_t kBatchWorkerMultiplier = 2;
constexpr std::string_view kFramePrefix = "frame_";
constexpr std::string_view kFrameSuffix = ".jpg";

}  // namespace

EquallySpacedExtractor::EquallySpacedExtractor(int num_samples) noexcept
    : num_samples_(num_samples) {}

bool EquallySpacedExtractor::run(const Ffmpeg& ff,
                                 const fs::path& video,
                                 const fs::path& video_out_dir) {
    const fs::path output_dir = video_out_dir / "image";
    fs::create_directories(output_dir);

    if (!extract(ff, video, output_dir)) {
        BOOST_LOG_TRIVIAL(warning)
            << "Warning: Failed to extract equally-spaced frames from "
            << core::to_utf8(video.filename());
        return false;
    }
    return true;
}

bool EquallySpacedExtractor::extract(const Ffmpeg& ff,
                                     const fs::path& video_file,
                                     const fs::path& output_dir) const {
    core::ScopedProfile total_prof("extract.equally_spaced.total");

    if (num_samples_ <= 0) {
        return false;
    }

    av::FrameSampler sampler;
    if (!sampler.open(video_file)) {
        BOOST_LOG_TRIVIAL(error)
            << "Error: Could not open video " << core::to_utf8(video_file);
        return false;
    }

    double duration = sampler.duration_sec();
    if (duration <= 0.0) {
        duration = ff.duration(video_file);
    }
    if (duration <= 0.0) {
        BOOST_LOG_TRIVIAL(error)
            << "Error: Could not determine duration of " << core::to_utf8(video_file);
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << std::format(
        "  Duration: {:.2f} s, extracting {} equally-spaced frames", duration, num_samples_);

    const auto sample_count = static_cast<std::size_t>(num_samples_);
    const double interval = duration / static_cast<double>(num_samples_);
    std::vector<double> timestamps;
    timestamps.reserve(sample_count);

    for (const int i : std::views::iota(0, num_samples_)) {
        double timestamp = (static_cast<double>(i) + kIntervalCenterOffset) * interval;
        if (timestamp >= duration) {
            timestamp = std::max(0.0, duration - kEndClampMarginSec);
        }
        timestamps.push_back(timestamp);
    }

    std::optional<core::ThreadPool> pool;
    std::size_t batch_size = kMinWriteBatchSize;
    if (sample_count > 1) {
        const std::size_t worker_count =
            std::min(sample_count, core::ThreadPool::default_thread_count());
        pool.emplace(worker_count);
        batch_size = std::max(kMinWriteBatchSize, pool->worker_count() * kBatchWorkerMultiplier);
    }

    bool all_ok = true;

    for (std::size_t offset = 0; offset < sample_count; offset += batch_size) {
        const std::size_t count = std::min(batch_size, sample_count - offset);
        const std::span<const double> batch_ts{timestamps.data() + offset, count};

        std::vector<av::FrameBuffer> decoded(count);
        {
            core::ScopedProfile decode_prof("extract.equally_spaced.decode");
            [[maybe_unused]] const std::size_t got = sampler.sample_timestamps(
                batch_ts, [&decoded](std::size_t local_idx, av::FrameBuffer&& fb) {
                    if (local_idx < decoded.size()) {
                        decoded[local_idx] = std::move(fb);
                    }
                });
        }

        std::vector<std::uint8_t> write_ok(count, 0U);
        auto write_one = [&decoded, &output_dir, &write_ok, offset](std::size_t local_idx) {
            if (decoded[local_idx].empty()) {
                return;
            }
            const fs::path output_file =
                output_dir /
                core::format_zero_padded(kFramePrefix,
                                         static_cast<int>(offset + local_idx) + 1,
                                         kFrameIndexWidth,
                                         kFrameSuffix);
            const cv::Mat view = extract_io::as_mat_view(decoded[local_idx]);
            write_ok[local_idx] = extract_io::write_jpeg(output_file, view) ? 1U : 0U;
        };

        {
            core::ScopedProfile write_prof("extract.equally_spaced.write");
            if (pool && count > 1) {
                pool->parallel_for(count, write_one);
            } else {
                for (const std::size_t local_idx : std::views::iota(std::size_t{0}, count)) {
                    write_one(local_idx);
                }
            }
        }

        for (const std::size_t local_idx : std::views::iota(std::size_t{0}, count)) {
            if (write_ok[local_idx] == 0U) {
                BOOST_LOG_TRIVIAL(warning)
                    << "  Warning: Failed to extract frame at "
                    << timestamps[offset + local_idx] << " s";
                all_ok = false;
            }
        }
    }

    const auto count = std::ranges::count_if(
        fs::directory_iterator(output_dir),
        [](const fs::directory_entry& entry) { return entry.is_regular_file(); });
    BOOST_LOG_TRIVIAL(info)
        << "  Extracted " << count << " frames to " << core::to_utf8(output_dir);

    return all_ok;
}
