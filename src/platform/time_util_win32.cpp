#include "platform/time_util.h"

namespace platform {

std::optional<std::tm> local_time(std::time_t t) noexcept {
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) {
        return std::nullopt;
    }
    return tm;
}

}  // namespace platform
