#pragma once

// MosaicBuilder -- composes striking frames from a detect directory into an
// HD mosaic image, using FingerprintCache to skip regeneration when unchanged.

#include <filesystem>

namespace fs = std::filesystem;

class MosaicBuilder {
public:
    // Creates a mosaic image from striking frames in detect_dir.
    // mosaic_start is 1-indexed: only frames from position mosaic_start onward
    // are considered.
    [[nodiscard]] bool build(const fs::path& detect_dir,
                             const fs::path& mosaic_dir,
                             int mosaic_count,
                             int mosaic_start) const;
};
