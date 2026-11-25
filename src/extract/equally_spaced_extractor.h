#pragma once

// ---------------------------------------------------------------------------
// EquallySpacedExtractor – FrameExtractor Strategy that writes N equally-spaced
// frames (frame_%04d.jpg) into the "image" subdir. Timestamps are clamped to
// duration-0.01 so exactly N frames are produced.
// ---------------------------------------------------------------------------

#include "extract/frame_extractor.h"

class EquallySpacedExtractor : public FrameExtractor {
public:
    explicit EquallySpacedExtractor(int num_samples);
    bool run(const Ffmpeg& ff, const fs::path& video, const fs::path& video_out_dir) override;

private:
    bool extract(const Ffmpeg& ff, const fs::path& video_file, const fs::path& output_dir);
    int num_samples_;
};
