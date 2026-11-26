#pragma once

// Turn libav* integer error codes into readable text. Every libav* entry point
// reports failure as a negative AVERROR; this module formats them consistently.

#include <string>

namespace av {

// Formats an AVERROR code as "<av_strerror text> (<errnum>)", e.g.
// "Invalid argument (-22)".
[[nodiscard]] std::string err_str(int errnum);

}  // namespace av
