#pragma once

// ---------------------------------------------------------------------------
// StrikingFrameExtractor – FrameExtractor Strategy that auto-detects visually
// striking frames (striking_%04d.jpg) into the "detect" subdir using the frame
// scorer, crop deduplicator and fingerprint cache. When a MosaicSpec is
// supplied, a mosaic is composed INSIDE run() but only after a successful
// detect — preserving the two distinct (striking vs mosaic) warnings.
// ---------------------------------------------------------------------------

#include "extract/frame_extractor.h"

#include <optional>

// Selection parameters for an optional mosaic composed from the striking frames.
struct MosaicSpec {
    int count;
    int start;
};

class StrikingFrameExtractor : public FrameExtractor {
public:
    explicit StrikingFrameExtractor(std::optional<MosaicSpec> mosaic);
    bool run(const Ffmpeg& ff, const fs::path& video, const fs::path& video_out_dir) override;

private:
    bool detect(const Ffmpeg& ff, const fs::path& video_file, const fs::path& output_dir);
    std::optional<MosaicSpec> mosaic_;
};
