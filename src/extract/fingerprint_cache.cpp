#include "extract/fingerprint_cache.h"

#include "core/format_util.h"
#include "core/path_util.h"
#include "core/scoped_handle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

namespace {

using EvpMdCtxPtr = core::UniqueHandle<EVP_MD_CTX, EVP_MD_CTX_free>;

constexpr std::size_t kReadBufferBytes = 65536;
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr unsigned int kHighNibbleShift = 4;
constexpr unsigned int kNibbleMask = 0x0FU;
constexpr std::size_t kHexCharsPerByte = 2;
constexpr std::string_view kJpgExtension = ".jpg";

[[nodiscard]] std::string bytes_to_hex(std::span<const unsigned char> bytes) {
    std::string hex;
    hex.reserve(bytes.size() * kHexCharsPerByte);
    for (const unsigned char byte : bytes) {
        hex.push_back(kHexDigits[(byte >> kHighNibbleShift) & kNibbleMask]);
        hex.push_back(kHexDigits[byte & kNibbleMask]);
    }
    return hex;
}

}  // namespace

std::string FingerprintCache::sha256_file(const fs::path& file_path) {
    std::error_code ec;
    if (!fs::is_regular_file(file_path, ec)) {
        return {};
    }

    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) {
        return {};
    }

    EvpMdCtxPtr ctx{EVP_MD_CTX_new()};
    if (!ctx) {
        return {};
    }
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        return {};
    }

    std::array<char, kReadBufferBytes> buf{};
    while (ifs.read(buf.data(), static_cast<std::streamsize>(buf.size())) || ifs.gcount() > 0) {
        const auto bytes_read = static_cast<std::size_t>(ifs.gcount());
        if (EVP_DigestUpdate(ctx.get(), buf.data(), bytes_read) != 1) {
            return {};
        }
    }
    if (ifs.bad()) {
        return {};
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash.data(), &hash_len) != 1) {
        return {};
    }

    const std::size_t digest_len = std::min<std::size_t>(hash_len, hash.size());
    return bytes_to_hex(std::span<const unsigned char>{hash.data(), digest_len});
}

std::string FingerprintCache::compute_dir_fingerprint(const fs::path& dir,
                                                      std::string_view prefix) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        return {};
    }

    fs::directory_iterator it(dir, ec);
    if (ec) {
        return {};
    }

    std::vector<std::string> entries;
    for (const auto& entry : it) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != kJpgExtension) {
            continue;
        }
        const std::string fname = core::to_utf8(entry.path().filename());
        if (!prefix.empty() && !fname.starts_with(prefix)) {
            continue;
        }
        entries.push_back(std::format("{}:{}", fname, sha256_file(entry.path())));
    }

    std::ranges::sort(entries);
    return core::join(entries, "\n");
}

void FingerprintCache::save(const fs::path& hash_file, std::string_view fingerprint) {
    std::ofstream ofs(hash_file);
    if (ofs.is_open()) {
        ofs << fingerprint;
    }
}

std::string FingerprintCache::load(const fs::path& hash_file) {
    std::error_code ec;
    if (!fs::is_regular_file(hash_file, ec)) {
        return {};
    }

    std::ifstream ifs(hash_file);
    if (!ifs.is_open()) {
        return {};
    }

    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}
