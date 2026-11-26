#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace test_support {

namespace fs = std::filesystem;

[[nodiscard]] inline std::string u8_to_utf8(std::u8string_view u8) {
    std::string out;
    out.reserve(u8.size());
    for (const char8_t ch : u8) {
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

[[nodiscard]] inline std::string path_to_utf8(const fs::path& p) {
    return u8_to_utf8(p.u8string());
}

[[nodiscard]] inline fs::path path_from_utf8(std::string_view utf8) {
    std::u8string u8;
    u8.reserve(utf8.size());
    for (const char ch : utf8) {
        u8.push_back(static_cast<char8_t>(static_cast<unsigned char>(ch)));
    }
    return fs::path{u8};
}

[[nodiscard]] inline fs::path path_from_u8(std::u8string_view u8) {
    return fs::path{u8};
}

[[nodiscard]] inline std::uint64_t current_pid() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

// Self-cleaning RAII temporary directory under std::filesystem::temp_directory_path().
// Guarantees zero leaked artifacts even when GoogleTest ASSERT_* macros abort early.
class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix = "ffmpeg_tools") {
        static std::atomic<std::uint64_t> counter{0};
        thread_local std::mt19937_64 rng{std::random_device{}()};
        const auto base = fs::temp_directory_path();
        const auto pid = current_pid();

        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
            const auto salt = rng() & 0xFFFF'FFFFULL;
            auto candidate = base / std::format("fftools_{}_{}_{}_{:08x}", prefix, pid, seq, salt);
            std::error_code ec;
            if (fs::create_directories(candidate, ec) && !ec) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("ScopedTempDir: failed to create unique temporary directory");
    }

    ~ScopedTempDir() noexcept { cleanup(); }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ScopedTempDir(ScopedTempDir&& other) noexcept : path_(std::exchange(other.path_, {})) {}

    ScopedTempDir& operator=(ScopedTempDir&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

    [[nodiscard]] fs::path child(std::string_view rel) const {
        return path_ / path_from_utf8(rel);
    }

    [[nodiscard]] fs::path child(std::u8string_view rel) const {
        return path_ / path_from_u8(rel);
    }

    [[nodiscard]] fs::path create_subdir(std::string_view rel) const {
        auto dir = child(rel);
        fs::create_directories(dir);
        return dir;
    }

    [[nodiscard]] fs::path create_subdir(std::u8string_view rel) const {
        auto dir = child(rel);
        fs::create_directories(dir);
        return dir;
    }

    fs::path write_bytes(std::string_view rel, std::span<const std::byte> data) const {
        auto target = child(rel);
        if (target.has_parent_path()) {
            fs::create_directories(target.parent_path());
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error(
                std::format("Failed to open for writing: {}", path_to_utf8(target)));
        }
        if (!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        }
        out.flush();
        if (!out) {
            throw std::runtime_error(
                std::format("Failed writing bytes to: {}", path_to_utf8(target)));
        }
        return target;
    }

    fs::path write_text(std::string_view rel, std::string_view text) const {
        const auto raw = std::as_bytes(std::span{text.data(), text.size()});
        return write_bytes(rel, raw);
    }

    fs::path copy_in(const fs::path& src, std::string_view dest_rel = {}) const {
        auto dst = dest_rel.empty() ? (path_ / src.filename()) : child(dest_rel);
        if (dst.has_parent_path()) {
            fs::create_directories(dst.parent_path());
        }
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        return dst;
    }

    fs::path copy_in(const fs::path& src, std::u8string_view dest_rel) const {
        auto dst = dest_rel.empty() ? (path_ / src.filename()) : child(dest_rel);
        if (dst.has_parent_path()) {
            fs::create_directories(dst.parent_path());
        }
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
        return dst;
    }

private:
    void cleanup() noexcept {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        fs::remove_all(path_, ec);
        path_.clear();
    }

    fs::path path_;
};

[[nodiscard]] inline std::vector<std::byte> read_file_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error(
            std::format("Failed to open for reading: {}", path_to_utf8(p)));
    }
    const auto end_pos = in.tellg();
    if (end_pos < 0) {
        return {};
    }
    std::vector<std::byte> buf(static_cast<std::size_t>(end_pos));
    in.seekg(0, std::ios::beg);
    if (!buf.empty()) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }
    return buf;
}

[[nodiscard]] inline std::string read_file_text(const fs::path& p) {
    const auto bytes = read_file_bytes(p);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] inline std::vector<std::byte> to_bytes(std::initializer_list<std::uint8_t> vals) {
    std::vector<std::byte> out;
    out.reserve(vals.size());
    for (const auto v : vals) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// Streaming 128-bit content digest (independent FNV-1a + Murmur-style avalanche lane)
// for fast byte-for-byte immutability checks without external crypto linkage.
struct ContentDigest {
    std::uint64_t size = 0;
    std::uint64_t h1 = 0;
    std::uint64_t h2 = 0;

    [[nodiscard]] bool operator==(const ContentDigest&) const = default;
};

[[nodiscard]] inline ContentDigest digest_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::format("digest_file: cannot open {}", path_to_utf8(p)));
    }
    constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    constexpr std::uint64_t kAltOffset = 0x9e3779b97f4a7c15ULL;
    constexpr std::uint64_t kAltPrime = 0xbf58476d1ce4e5b9ULL;

    ContentDigest d{0, kFnvOffset, kAltOffset};
    std::array<char, 65536> chunk{};
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = static_cast<std::size_t>(in.gcount());
        if (count == 0) {
            break;
        }
        d.size += count;
        for (std::size_t i = 0; i < count; ++i) {
            const auto b = static_cast<std::uint8_t>(chunk[i]);
            d.h1 ^= b;
            d.h1 *= kFnvPrime;
            d.h2 = (d.h2 ^ (static_cast<std::uint64_t>(b) + (d.size * 0x9e3779b9ULL))) * kAltPrime;
            d.h2 ^= (d.h2 >> 31);
        }
    }
    return d;
}

class FileSnapshot {
public:
    explicit FileSnapshot(fs::path p)
        : path_(std::move(p)), digest_(digest_file(path_)) {}

    [[nodiscard]] bool is_unchanged() const {
        std::error_code ec;
        if (!fs::is_regular_file(path_, ec) || ec) {
            return false;
        }
        return digest_file(path_) == digest_;
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    [[nodiscard]] const ContentDigest& digest() const noexcept { return digest_; }

private:
    fs::path path_;
    ContentDigest digest_;
};

[[nodiscard]] inline std::vector<fs::path> list_regular_files(
    const fs::path& dir, std::string_view extension_filter = {},
    std::string_view filename_prefix = {}) {
    std::vector<fs::path> result;
    if (!fs::exists(dir)) {
        return result;
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto& p = entry.path();
        if (!extension_filter.empty() && path_to_utf8(p.extension()) != extension_filter) {
            continue;
        }
        if (!filename_prefix.empty() &&
            !path_to_utf8(p.filename()).starts_with(filename_prefix)) {
            continue;
        }
        result.push_back(p);
    }
    std::ranges::sort(result, [](const fs::path& a, const fs::path& b) {
        return path_to_utf8(a.filename()) < path_to_utf8(b.filename());
    });
    return result;
}

[[nodiscard]] inline std::vector<fs::path> list_all_entries(const fs::path& dir) {
    std::vector<fs::path> result;
    if (!fs::exists(dir)) {
        return result;
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        result.push_back(entry.path());
    }
    std::ranges::sort(result, [](const fs::path& a, const fs::path& b) {
        return path_to_utf8(a.filename()) < path_to_utf8(b.filename());
    });
    return result;
}

// Pins a file's modification time cleanly into the past so cache-hit tests can prove
// zero rewrites deterministically with zero std::this_thread::sleep_for() calls.
[[nodiscard]] inline fs::file_time_type pin_mtime_in_past(
    const fs::path& p, std::chrono::seconds age = std::chrono::seconds{120}) {
    const auto target = fs::file_time_type::clock::now() - age;
    fs::last_write_time(p, target);
    return fs::last_write_time(p);
}

[[nodiscard]] inline fs::path fixture_path(std::string_view rel) {
    return path_from_utf8(TEST_DATA_DIR) / path_from_utf8(rel);
}

inline constexpr std::string_view kFixtureMainVideo = "2026-03-22 00-16-33.mp4";
inline constexpr std::string_view kFixtureMixedFpsDir = "fps_mixed";
inline constexpr std::string_view kFixtureUniformFpsDir = "fps_uniform";
inline constexpr std::string_view kFixtureUniformClip1 = "fps_uniform/2026-01-01 00-00-00.mp4";
inline constexpr std::string_view kFixtureUniformClip2 = "fps_uniform/2026-01-02 00-00-00.mp4";

}  // namespace test_support
