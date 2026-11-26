#pragma once

// FrameExtractor -- Strategy protocol for frame extraction modes.
// Each concrete extractor creates its own subdirectory (image/detect/text)
// under video_out_dir, logs warnings on failure, and returns true on success.

#include <filesystem>

namespace fs = std::filesystem;

class Ffmpeg;

class FrameExtractor {
public:
    virtual ~FrameExtractor();

    FrameExtractor(const FrameExtractor&) = delete;
    FrameExtractor& operator=(const FrameExtractor&) = delete;

    [[nodiscard]] virtual bool run(const Ffmpeg& ff,
                                   const fs::path& video,
                                   const fs::path& video_out_dir) = 0;

protected:
    FrameExtractor() = default;
    FrameExtractor(FrameExtractor&&) noexcept = default;
    FrameExtractor& operator=(FrameExtractor&&) noexcept = default;
};
