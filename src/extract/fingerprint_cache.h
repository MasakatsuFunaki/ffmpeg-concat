#pragma once

// FingerprintCache -- SHA-256 content fingerprinting for directory caching.

#include <filesystem>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

class FingerprintCache {
public:
    // Computes the lowercase hex SHA-256 digest of file_path, or "" on failure.
    [[nodiscard]] static std::string sha256_file(const fs::path& file_path);

    // Computes sorted "name:sha256" lines for .jpg files in dir matching prefix.
    [[nodiscard]] static std::string compute_dir_fingerprint(const fs::path& dir,
                                                             std::string_view prefix = {});

    static void save(const fs::path& hash_file, std::string_view fingerprint);

    [[nodiscard]] static std::string load(const fs::path& hash_file);
};
