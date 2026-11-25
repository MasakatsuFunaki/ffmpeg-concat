#pragma once

// ---------------------------------------------------------------------------
// av::grab_frame – decode one frame at a timestamp and write it as an image.
//
// The format is chosen from the output file's extension, exactly as the muxer
// would; .jpg is what the extract tool asks for.
// ---------------------------------------------------------------------------

#include <filesystem>

namespace fs = std::filesystem;

namespace av {

// Seek to `timestamp_sec`, decode the first frame at or after it, and write it
// to `out_image`. Returns true on success. Never throws.
bool grab_frame(const fs::path& in, const fs::path& out_image, double timestamp_sec);

}  // namespace av
