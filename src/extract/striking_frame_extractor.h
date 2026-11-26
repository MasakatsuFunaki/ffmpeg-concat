#pragma once

// StrikingFrameExtractor -- FrameExtractor Strategy that auto-detects visually
// striking frames (striking_%04d.jpg) into the "detect" subdir and optionally
// composes a mosaic into the "mosaics" subdir after a successful detect pass.

#include "extract/frame_extractor.h"

#include <optional>

struct MosaicSpec {
    int count = 0;
    int start = 1;

    [[nodiscard]] constexpr bool operator==(const MosaicSpec&) const noexcept = default;
};

class StrikingFrameExtractor final : public FrameExtractor {
public:
    explicit StrikingFrameExtractor(std::optional<MosaicSpec> mosaic) noexcept;

    [[nodiscard]] bool run(const Ffmpeg& ff,
                           const fs::path& video,
                           const fs::path& video_out_dir) override;

private:
    [[nodiscard]] bool detect(const Ffmpeg& ff,
                              const fs::path& video_file,
                              const fs::path& output_dir) const;

    std::optional<MosaicSpec> mosaic_;
};
