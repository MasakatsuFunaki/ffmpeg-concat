#include "platform/cli_args.h"

#include <cstddef>
#include <string>
#include <vector>

namespace platform {

std::vector<std::string> utf8_command_line(int argc, char* argv[]) {
    if (argc <= 0 || argv == nullptr) {
        return {};
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i] ? argv[i] : "");
    }
    return args;
}

}  // namespace platform
