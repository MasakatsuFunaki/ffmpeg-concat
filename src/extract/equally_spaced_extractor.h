#pragma once

// EquallySpacedExtractor -- FrameExtractor Strategy that extracts N equally
// spaced frames (frame_%04d.jpg) into the "image" subdirectory.

#include "extract/frame_extractor.h"

class EquallySpacedExtractor final : public FrameExtractor {
public:
    explicit EquallySpacedExtractor(int num_samples) noexcept;

    [[nodiscard]] bool run(const Ffmpeg& ff,
                           const fs::path& video,
                           const fs::path& video_out_dir) override;

private:
    [[nodiscard]] bool extract(const Ffmpeg& ff,
                               const fs::path& video_file,
                               const fs::path& output_dir) const;

    int num_samples_;
};
