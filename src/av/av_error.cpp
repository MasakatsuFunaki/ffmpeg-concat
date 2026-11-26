#include "av/av_error.h"

#include <array>
#include <format>
#include <string_view>

extern "C" {
#include <libavutil/error.h>
}

namespace av {

std::string err_str(int errnum) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
    if (av_strerror(errnum, buf.data(), buf.size()) < 0) {
        return std::format("unknown error ({})", errnum);
    }
    return std::format("{} ({})", std::string_view{buf.data()}, errnum);
}

}  // namespace av
