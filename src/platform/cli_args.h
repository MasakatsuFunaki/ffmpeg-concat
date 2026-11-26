#pragma once

// ---------------------------------------------------------------------------
// platform/cli_args.h – Acquire process command-line arguments as UTF-8.
//
// On Windows, retrieves the native UTF-16 command line via GetCommandLineW /
// CommandLineToArgvW and encodes each argument as UTF-8 so non-ASCII paths
// survive regardless of the system ANSI code page. On POSIX, copies argv
// verbatim. Platform implementation files are selected by CMake.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace platform {

// Returns the process command-line arguments encoded as UTF-8.
// Falls back to copying (argc, argv) if native retrieval fails, or returns
// an empty vector when argc <= 0 or argv == nullptr.
[[nodiscard]] std::vector<std::string> utf8_command_line(int argc, char* argv[]);

}  // namespace platform
