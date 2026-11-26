#pragma once

// TextCropExtractor -- FrameExtractor Strategy that samples frames into the
// "text" subdir, detects text regions, runs the CropFilter chain, deduplicates
// survivors, and writes text_%04d.jpg. Uses PIMPL to insulate callers from
// OpenCV headers.

#include "extract/frame_extractor.h"

#include <memory>

class TextCropExtractor final : public FrameExtractor {
public:
    TextCropExtractor();
    ~TextCropExtractor() override;

    TextCropExtractor(const TextCropExtractor&) = delete;
    TextCropExtractor& operator=(const TextCropExtractor&) = delete;
    TextCropExtractor(TextCropExtractor&&) noexcept;
    TextCropExtractor& operator=(TextCropExtractor&&) noexcept;

    [[nodiscard]] bool run(const Ffmpeg& ff,
                           const fs::path& video,
                           const fs::path& video_out_dir) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
