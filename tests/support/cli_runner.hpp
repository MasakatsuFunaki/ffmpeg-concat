#pragma once

#include <array>
#include <chrono>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace test_support {

namespace fs = std::filesystem;

template <typename App>
concept RunnableToolApp = requires(App& app, int argc, char** argv) {
    { app.run(argc, argv) } -> std::same_as<int>;
};

// Invokes a ToolApp subclass in-process through its public run(argc, argv) boundary.
template <RunnableToolApp App>
[[nodiscard]] inline int run_tool_app(std::span<const std::string> args) {
    std::vector<std::string> owned(args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& s : owned) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);
    App app;
    return app.run(static_cast<int>(owned.size()), argv.data());
}

template <RunnableToolApp App>
[[nodiscard]] inline int run_tool_app(std::initializer_list<std::string_view> args) {
    std::vector<std::string> owned;
    owned.reserve(args.size());
    for (const auto sv : args) {
        owned.emplace_back(sv);
    }
    return run_tool_app<App>(std::span<const std::string>{owned});
}

[[nodiscard]] inline std::optional<std::string> get_env_var(std::string_view name) {
    const std::string key(name);
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&raw, &len, key.c_str()) != 0 || raw == nullptr) {
        return std::nullopt;
    }
    std::string value(raw);
    std::free(raw);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
#else
    const char* raw = std::getenv(key.c_str());
    if (raw == nullptr || raw[0] == '\0') {
        return std::nullopt;
    }
    return std::string(raw);
#endif
}

// Resolves a compiled CLI executable for ST runs.
// Honors FFMPEG_TOOLS_BIN_DIR when set (e.g. pointing at staged install/win or install/linux);
// otherwise falls back to the CMake $<TARGET_FILE:...> path baked into the ST binary.
[[nodiscard]] inline fs::path resolve_tool_executable(
    std::string_view tool_name, std::string_view default_target_path) {
    if (const auto override_dir = get_env_var("FFMPEG_TOOLS_BIN_DIR")) {
#ifdef _WIN32
        return fs::path(*override_dir) / (std::string(tool_name) + ".exe");
#else
        return fs::path(*override_dir) / std::string(tool_name);
#endif
    }
    return fs::path(default_target_path);
}

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string stdout_text;
    std::string stderr_text;
};

struct ProcessOptions {
    std::optional<fs::path> working_dir;
    bool clear_path = false;
    std::chrono::milliseconds timeout{120000};
};

#ifdef _WIN32
namespace detail {

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : h_(std::exchange(other.h_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            h_ = std::exchange(other.h_, nullptr);
        }
        return *this;
    }

    void reset(HANDLE h = nullptr) noexcept {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h_);
        }
        h_ = h;
    }

    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_ = nullptr;
};

[[nodiscard]] inline std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed");
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

// Quotes an argument following MSVC CommandLineToArgvW parsing rules.
[[nodiscard]] inline std::wstring quote_win_arg(std::wstring_view arg) {
    const bool needs_quotes = arg.empty() ||
        arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) {
        return std::wstring(arg);
    }
    std::wstring out;
    out.push_back(L'"');
    for (auto it = arg.begin(); it != arg.end(); ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++backslashes;
            ++it;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

inline void drain_win_pipe(HANDLE h, std::string& dst) {
    std::array<char, 4096> buf{};
    DWORD nread = 0;
    while (::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &nread, nullptr) && nread > 0) {
        dst.append(buf.data(), static_cast<std::size_t>(nread));
    }
}

[[nodiscard]] inline std::vector<wchar_t> build_cleared_path_env() {
    std::vector<wchar_t> block;
    wchar_t* raw_env = ::GetEnvironmentStringsW();
    if (raw_env != nullptr) {
        const wchar_t* cur = raw_env;
        while (*cur != L'\0') {
            const std::wstring_view entry(cur);
            const auto eq = entry.find(L'=');
            bool is_path = false;
            if (eq == 4) {
                const auto key = entry.substr(0, 4);
                is_path = (key[0] == L'P' || key[0] == L'p') &&
                          (key[1] == L'A' || key[1] == L'a') &&
                          (key[2] == L'T' || key[2] == L't') &&
                          (key[3] == L'H' || key[3] == L'h');
            }
            if (!is_path) {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(L'\0');
            }
            cur += entry.size() + 1;
        }
        ::FreeEnvironmentStringsW(raw_env);
    }
    const std::wstring_view empty_path = L"PATH=";
    block.insert(block.end(), empty_path.begin(), empty_path.end());
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

}  // namespace detail

[[nodiscard]] inline ProcessResult run_process(
    const fs::path& exe, std::span<const std::string> args, const ProcessOptions& opts = {}) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE raw_out_r = nullptr, raw_out_w = nullptr;
    HANDLE raw_err_r = nullptr, raw_err_w = nullptr;
    if (!::CreatePipe(&raw_out_r, &raw_out_w, &sa, 0) ||
        !::CreatePipe(&raw_err_r, &raw_err_w, &sa, 0)) {
        throw std::runtime_error("run_process: CreatePipe failed");
    }
    detail::UniqueHandle out_r(raw_out_r), out_w(raw_out_w);
    detail::UniqueHandle err_r(raw_err_r), err_w(raw_err_w);
    ::SetHandleInformation(out_r.get(), HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_r.get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdline = detail::quote_win_arg(exe.wstring());
    for (const auto& arg : args) {
        cmdline.push_back(L' ');
        cmdline.append(detail::quote_win_arg(detail::utf8_to_wide(arg)));
    }
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_w.get();
    si.hStdError = err_w.get();
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> env_block;
    void* env_ptr = nullptr;
    DWORD flags = CREATE_NO_WINDOW;
    if (opts.clear_path) {
        env_block = detail::build_cleared_path_env();
        env_ptr = env_block.data();
        flags |= CREATE_UNICODE_ENVIRONMENT;
    }

    const std::wstring cwd_w = opts.working_dir ? opts.working_dir->wstring() : std::wstring{};
    const wchar_t* cwd_ptr = opts.working_dir ? cwd_w.c_str() : nullptr;

    const BOOL ok = ::CreateProcessW(
        exe.c_str(),
        cmd_buf.data(),
        nullptr,
        nullptr,
        TRUE,
        flags,
        env_ptr,
        cwd_ptr,
        &si,
        &pi);

    out_w.reset();
    err_w.reset();

    if (!ok) {
        throw std::runtime_error("run_process: CreateProcessW failed for " + exe.string());
    }
    detail::UniqueHandle proc(pi.hProcess);
    detail::UniqueHandle thr(pi.hThread);

    ProcessResult result;
    {
        std::jthread t_out([&] { detail::drain_win_pipe(out_r.get(), result.stdout_text); });
        std::jthread t_err([&] { detail::drain_win_pipe(err_r.get(), result.stderr_text); });

        const DWORD wait_ms = static_cast<DWORD>(opts.timeout.count());
        const DWORD wait_rc = ::WaitForSingleObject(proc.get(), wait_ms);
        if (wait_rc == WAIT_TIMEOUT) {
            result.timed_out = true;
            ::TerminateProcess(proc.get(), 124);
            ::WaitForSingleObject(proc.get(), 5000);
        }
    }

    DWORD exit_code = 0;
    if (::GetExitCodeProcess(proc.get(), &exit_code)) {
        result.exit_code = static_cast<int>(exit_code);
    }
    return result;
}

#else

namespace detail {

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() noexcept { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

inline void drain_posix_fd(int fd, std::string& dst) {
    std::array<char, 4096> buf{};
    for (;;) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) {
            dst.append(buf.data(), static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
}

}  // namespace detail

[[nodiscard]] inline ProcessResult run_process(
    const fs::path& exe, std::span<const std::string> args, const ProcessOptions& opts = {}) {
    int out_pipe[2]{-1, -1};
    int err_pipe[2]{-1, -1};
    int exit_pipe[2]{-1, -1};
    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0 || ::pipe(exit_pipe) != 0) {
        throw std::runtime_error("run_process: pipe() failed");
    }
    detail::UniqueFd out_r(out_pipe[0]), out_w(out_pipe[1]);
    detail::UniqueFd err_r(err_pipe[0]), err_w(err_pipe[1]);
    detail::UniqueFd exit_r(exit_pipe[0]), exit_w(exit_pipe[1]);
    // Keep exit_w open inside the child (do NOT mark FD_CLOEXEC) so exit_r hits
    // EOF/POLLHUP the exact instant the child terminates.
    ::fcntl(exit_r.get(), F_SETFD, FD_CLOEXEC);

    std::vector<std::string> argv_storage;
    argv_storage.reserve(args.size() + 1);
    argv_storage.push_back(exe.string());
    argv_storage.insert(argv_storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& s : argv_storage) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    std::vector<char*> envp;
    if (opts.clear_path && environ != nullptr) {
        for (char** ep = environ; *ep != nullptr; ++ep) {
            const std::string_view entry(*ep);
            if (!entry.starts_with("PATH=")) {
                env_storage.emplace_back(entry);
            }
        }
        env_storage.emplace_back("PATH=");
        envp.reserve(env_storage.size() + 1);
        for (auto& s : env_storage) {
            envp.push_back(s.data());
        }
        envp.push_back(nullptr);
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error("run_process: fork() failed");
    }
    if (pid == 0) {
        ::dup2(out_w.get(), STDOUT_FILENO);
        ::dup2(err_w.get(), STDERR_FILENO);
        out_r.reset();
        out_w.reset();
        err_r.reset();
        err_w.reset();
        exit_r.reset();
        if (opts.working_dir) {
            if (::chdir(opts.working_dir->c_str()) != 0) {
                ::_exit(127);
            }
        }
        if (opts.clear_path) {
            ::execve(exe.c_str(), argv.data(), envp.data());
        } else {
            ::execv(exe.c_str(), argv.data());
        }
        ::_exit(127);
    }

    out_w.reset();
    err_w.reset();
    exit_w.reset();

    ProcessResult result;
    {
        std::jthread t_out([&] { detail::drain_posix_fd(out_r.get(), result.stdout_text); });
        std::jthread t_err([&] { detail::drain_posix_fd(err_r.get(), result.stderr_text); });

        // Wait deterministically on child exit via exit_r EOF/POLLHUP with bounded timeout.
        const auto deadline = std::chrono::steady_clock::now() + opts.timeout;
        struct pollfd pfd{exit_r.get(), POLLIN | POLLHUP, 0};
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                result.timed_out = true;
                ::kill(pid, SIGKILL);
                break;
            }
            const auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const int prc = ::poll(&pfd, 1, static_cast<int>(rem_ms > 0 ? rem_ms : 1));
            if (prc > 0) {
                break;
            }
            if (prc == 0) {
                result.timed_out = true;
                ::kill(pid, SIGKILL);
                break;
            }
            if (errno != EINTR) {
                break;
            }
        }
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (result.timed_out) {
            result.exit_code = 124;
        } else if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exit_code = 128 + WTERMSIG(status);
        }
    }
    return result;
}
#endif

[[nodiscard]] inline ProcessResult run_process(
    const fs::path& exe, std::initializer_list<std::string_view> args,
    const ProcessOptions& opts = {}) {
    std::vector<std::string> owned;
    owned.reserve(args.size());
    for (const auto sv : args) {
        owned.emplace_back(sv);
    }
    return run_process(exe, std::span<const std::string>{owned}, opts);
}

}  // namespace test_support
