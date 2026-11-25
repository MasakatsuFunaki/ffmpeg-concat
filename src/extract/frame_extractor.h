#pragma once

// ---------------------------------------------------------------------------
// FrameExtractor – extraction Strategy (GoF "Strategy" pattern). Each concrete
// extractor owns one extraction mode: it creates its own subdirectory
// (image/detect/text) under video_out_dir first, self-logs its verbatim
// warning(s) on failure, and returns the same bool the original per-mode
// function returned. The extract tool holds a vector of these and runs them
// in mode order.
// ---------------------------------------------------------------------------

#include <filesystem>

namespace fs = std::filesystem;

// Forward-declared so extractor headers stay free of the ffmpeg facade's
// transitive includes; the full definition lives in ffmpeg/ffmpeg.h.
class Ffmpeg;

class FrameExtractor {
public:
    virtual ~FrameExtractor() = default;
    virtual bool run(const Ffmpeg& ff, const fs::path& video, const fs::path& video_out_dir) = 0;
};
