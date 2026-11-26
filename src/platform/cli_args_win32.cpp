#include "platform/cli_args.h"

#include "core/scoped_handle.h"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace platform {

namespace {

[[nodiscard]] std::vector<std::string> copy_fallback(int argc, char* argv[]) {
    if (argc <= 0 || argv == nullptr) {
        return {};
    }
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(argv[i] ? argv[i] : "");
    }
    return out;
}

[[nodiscard]] std::string wide_to_utf8(std::wstring_view wide) {
    constexpr auto kMaxInt = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    if (wide.empty() || wide.size() > kMaxInt) {
        return {};
    }
    const int src_len = static_cast<int>(wide.size());
    const int bytes_needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), src_len, nullptr, 0, nullptr, nullptr);
    if (bytes_needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes_needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), src_len, out.data(), bytes_needed, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

}  // namespace

std::vector<std::string> utf8_command_line(int argc, char* argv[]) {
    LPCWSTR const cmd_line = ::GetCommandLineW();
    if (!cmd_line) {
        return copy_fallback(argc, argv);
    }

    int wargc = 0;
    LPWSTR* const wargv = ::CommandLineToArgvW(cmd_line, &wargc);
    if (!wargv) {
        return copy_fallback(argc, argv);
    }
    const auto free_wargv = core::make_scope_guard([wargv]() noexcept {
        ::LocalFree(wargv);
    });
    if (wargc <= 0) {
        return copy_fallback(argc, argv);
    }

    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<std::size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
        const std::wstring_view arg = wargv[i] ? std::wstring_view{wargv[i]} : std::wstring_view{};
        utf8_args.push_back(wide_to_utf8(arg));
    }
    return utf8_args;
}

}  // namespace platform
