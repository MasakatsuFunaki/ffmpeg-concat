#pragma once

// Single-frame extraction: decode one frame at a timestamp and encode it to an
// image whose format is inferred from the output file extension (e.g. .jpg).

#include <filesystem>

namespace fs = std::filesystem;

namespace av {

// Seeks to `timestamp_sec`, decodes the first frame at or after it, and writes
// it to `out_image`. Returns true on success. Never throws.
[[nodiscard]] bool grab_frame(const fs::path& in,
                              const fs::path& out_image,
                              double timestamp_sec);

}  // namespace av
