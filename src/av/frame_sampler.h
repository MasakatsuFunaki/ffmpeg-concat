#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace fs = std::filesystem;

namespace av {

struct FrameBuffer {
    int width = 0;
    int height = 0;
    int stride = 0;                     // bytes per row (width * 3 for tightly packed BGR24)
    double timestamp_sec = 0.0;         // actual decoded frame PTS in seconds (or requested ts if NOPTS)
    std::vector<std::uint8_t> bgr24;    // tightly packed BGR24 pixels (height * stride bytes)

    [[nodiscard]] bool empty() const noexcept {
        return width <= 0 || height <= 0 || stride <= 0 || bgr24.empty();
    }
};

// Reusable video frame decoder and BGR24 converter for sampling multiple
// timestamps from a single media file without reopening the container or codec.
// Thread-compatible: concurrent access requires external synchronization; create
// one FrameSampler instance per worker thread.
class FrameSampler {
public:
    FrameSampler();
    ~FrameSampler();

    FrameSampler(const FrameSampler&) = delete;
    FrameSampler& operator=(const FrameSampler&) = delete;
    FrameSampler(FrameSampler&&) noexcept;
    FrameSampler& operator=(FrameSampler&&) noexcept;

    // Opens video_path (UTF-8 safe via core::to_utf8), video decoder, and reusable
    // buffers once. Returns false on missing/corrupt/audio-only files. Never throws.
    [[nodiscard]] bool open(const fs::path& video_path) noexcept;
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] double duration_sec() const noexcept;
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;

    // Decodes the first frame at or after timestamp_sec into tightly packed BGR24.
    // Returns std::nullopt if closed, timestamp_sec is non-finite/negative/past EOF,
    // or decode fails. Never throws.
    [[nodiscard]] std::optional<FrameBuffer> sample_at(double timestamp_sec) noexcept;

    // Samples all requested timestamps in order, invoking callback(index, std::move(frame))
    // for each successfully decoded frame. Returns the number of decoded frames.
    [[nodiscard]] std::size_t sample_timestamps(
        std::span<const double> timestamps_sec,
        const std::function<void(std::size_t, FrameBuffer&&)>& callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace av
