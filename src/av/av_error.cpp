#include "av/av_error.h"

extern "C" {
#include <libavutil/error.h>
}

namespace av {

std::string err_str(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(errnum, buf, sizeof(buf)) < 0) {
        return "unknown error (" + std::to_string(errnum) + ")";
    }
    return std::string(buf) + " (" + std::to_string(errnum) + ")";
}

}  // namespace av
