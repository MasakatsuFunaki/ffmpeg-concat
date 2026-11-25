#pragma once

// ---------------------------------------------------------------------------
// MosaicBuilder – plain class (deliberately NOT a pattern) that composes the
// striking frames of a detect directory into a single HD mosaic image, with a
// fingerprint cache so an unchanged input set skips regeneration.
// ---------------------------------------------------------------------------

#include <filesystem>

namespace fs = std::filesystem;

class MosaicBuilder {
public:
    // Create a mosaic image from striking frames in detect_dir.
    // mosaic_start is 1-indexed: only consider frames from position mosaic_start onward.
    bool build(const fs::path& detect_dir,
               const fs::path& mosaic_dir,
               int mosaic_count,
               int mosaic_start) const;
};
