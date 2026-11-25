#pragma once

// ---------------------------------------------------------------------------
// av_error – turn libav* integer error codes into readable text.
//
// Every libav* entry point reports failure as a negative AVERROR; this is the
// one place that formats them, so error messages read the same everywhere.
// ---------------------------------------------------------------------------

#include <string>

namespace av {

// "Invalid argument (-22)" — av_strerror() text plus the raw code.
std::string err_str(int errnum);

}  // namespace av
