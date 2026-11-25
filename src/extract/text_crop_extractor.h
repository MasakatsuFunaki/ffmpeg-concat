#pragma once

// ---------------------------------------------------------------------------
// TextCropExtractor – FrameExtractor Strategy that samples one frame per second
// into the "text" subdir, detects text regions, runs each candidate crop
// through the CropFilter chain, deduplicates the survivors and writes them as
// text_%04d.jpg. Owns the ordered CropFilter chain so the original short-circuit
// evaluation order is preserved.
// ---------------------------------------------------------------------------

#include "extract/frame_extractor.h"
#include "extract/crop_filter.h"
#include "extract/text_region_detector.h"

#include <memory>
#include <vector>

class TextCropExtractor : public FrameExtractor {
public:
    TextCropExtractor();
    bool run(const Ffmpeg& ff, const fs::path& video, const fs::path& video_out_dir) override;

private:
    bool extract(const Ffmpeg& ff, const fs::path& video_file, const fs::path& output_dir);
    std::vector<std::unique_ptr<CropFilter>> filters_;
    TextRegionDetector detector_;
};
