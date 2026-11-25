#pragma once

// ---------------------------------------------------------------------------
// FingerprintCache – plain utility (deliberately NOT a Proxy/Decorator) that
// fingerprints the jpg contents of a directory so detect/mosaic runs can skip
// work when their inputs are unchanged. Just SHA-256 hashing plus save/load of
// the fingerprint file.
// ---------------------------------------------------------------------------

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

class FingerprintCache {
public:
    // Compute SHA-256 hash of a file's contents, returns hex string.
    static std::string sha256_file(const fs::path& file_path);

    // Compute a fingerprint from jpg files in a directory (sorted name:sha256 pairs).
    static std::string compute_dir_fingerprint(const fs::path& dir,
                                               const std::string& prefix = "");

    // Save/load a fingerprint to/from a hash file.
    static void save(const fs::path& hash_file, const std::string& fingerprint);
    static std::string load(const fs::path& hash_file);
};
