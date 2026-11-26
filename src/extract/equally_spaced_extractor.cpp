#include "extract/equally_spaced_extractor.h"

#include "core/format_util.h"
#include "ffmpeg/ffmpeg.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>

#include <boost/log/trivial.hpp>

namespace {

constexpr double kIntervalCenterOffset = 0.5;
constexpr double kEndClampMarginSec = 0.01;
constexpr int kFrameIndexWidth = 4;
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
            << "Warning: Failed to extract equally-spaced frames from " << video.filename();
        return false;
    }
    return true;
}

bool EquallySpacedExtractor::extract(const Ffmpeg& ff,
                                     const fs::path& video_file,
                                     const fs::path& output_dir) const {
    if (num_samples_ <= 0) {
        return false;
    }

    const double duration = ff.duration(video_file);
    if (duration <= 0.0) {
        BOOST_LOG_TRIVIAL(error) << "Error: Could not determine duration of " << video_file;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << std::format(
        "  Duration: {:.2f} s, extracting {} equally-spaced frames", duration, num_samples_);

    const double interval = duration / static_cast<double>(num_samples_);
    bool all_ok = true;

    for (const int i : std::views::iota(0, num_samples_)) {
        double timestamp = (static_cast<double>(i) + kIntervalCenterOffset) * interval;
        if (timestamp >= duration) {
            timestamp = std::max(0.0, duration - kEndClampMarginSec);
        }

        const fs::path output_file =
            output_dir /
            core::format_zero_padded(kFramePrefix, i + 1, kFrameIndexWidth, kFrameSuffix);

        if (!ff.extract_frame(video_file, output_file, timestamp)) {
            BOOST_LOG_TRIVIAL(warning)
                << "  Warning: Failed to extract frame at " << timestamp << " s";
            all_ok = false;
        }
    }

    const auto count = std::ranges::count_if(
        fs::directory_iterator(output_dir),
        [](const fs::directory_entry& entry) { return entry.is_regular_file(); });
    BOOST_LOG_TRIVIAL(info) << "  Extracted " << count << " frames to " << output_dir;

    return all_ok;
}
