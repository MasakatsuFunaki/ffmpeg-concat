#pragma once

// extract/image_io.h -- Unicode-safe file image I/O and zero-copy FrameBuffer
// cv::Mat views for extractcore.

#include "av/frame_sampler.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <system_error>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

namespace extract_io {

inline constexpr int kDefaultJpegQuality = 95;
inline constexpr std::size_t kBgrChannelCount = 3;

// Reads and decodes an image via std::ifstream + cv::imdecode so non-ASCII
// paths work on Windows regardless of the process ANSI codepage.
// Never throws; returns an empty cv::Mat on any I/O or decode failure.
[[nodiscard]] inline cv::Mat read_image(const std::filesystem::path& path,
                                        int flags = cv::IMREAD_COLOR) noexcept {
    try {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) {
            return {};
        }

        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs.is_open()) {
            return {};
        }

        const std::streampos end_pos = ifs.tellg();
        if (end_pos == std::streampos(-1)) {
            return {};
        }
        const std::streamoff byte_count = static_cast<std::streamoff>(end_pos);
        if (byte_count <= 0) {
            return {};
        }

        ifs.seekg(0, std::ios::beg);
        if (!ifs) {
            return {};
        }

        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(byte_count));
        if (!ifs.read(reinterpret_cast<char*>(buffer.data()),
                      static_cast<std::streamsize>(byte_count))) {
            return {};
        }

        return cv::imdecode(buffer, flags);
    } catch (...) {
        return {};
    }
}

// Encodes image as JPEG in memory and writes via std::ofstream so non-ASCII
// paths work on Windows regardless of the process ANSI codepage.
// Returns true iff encoding, writing, and flushing all succeeded.
[[nodiscard]] inline bool write_jpeg(const std::filesystem::path& path,
                                     const cv::Mat& image,
                                     int quality = kDefaultJpegQuality) noexcept {
    try {
        if (image.empty() || image.cols <= 0 || image.rows <= 0) {
            return false;
        }

        std::vector<std::uint8_t> encoded;
        const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, quality};
        if (!cv::imencode(".jpg", image, encoded, params) || encoded.empty()) {
            return false;
        }
        if (encoded.size() >
            static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            return false;
        }

        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return false;
        }

        ofs.write(reinterpret_cast<const char*>(encoded.data()),
                  static_cast<std::streamsize>(encoded.size()));
        ofs.flush();
        return ofs.good();
    } catch (...) {
        return false;
    }
}

// Returns a non-owning CV_8UC3 view over fb.bgr24 (zero copy).
// REQUIRES: fb outlives the returned cv::Mat and callers treat the view as read-only.
[[nodiscard]] inline cv::Mat as_mat_view(const av::FrameBuffer& fb) {
    if (fb.empty() || fb.width <= 0 || fb.height <= 0 || fb.stride <= 0) {
        return {};
    }

    const std::size_t min_row_bytes =
        static_cast<std::size_t>(fb.width) * kBgrChannelCount;
    const std::size_t stride_bytes = static_cast<std::size_t>(fb.stride);
    if (stride_bytes < min_row_bytes) {
        return {};
    }

    const std::size_t required_bytes =
        stride_bytes * static_cast<std::size_t>(fb.height - 1) + min_row_bytes;
    if (fb.bgr24.size() < required_bytes) {
        return {};
    }

    return cv::Mat(fb.height, fb.width, CV_8UC3,
                   const_cast<std::uint8_t*>(fb.bgr24.data()), stride_bytes);
}

}  // namespace extract_io
