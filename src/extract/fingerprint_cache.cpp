#include "extract/fingerprint_cache.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <vector>

#include <openssl/evp.h>

std::string FingerprintCache::sha256_file(const fs::path& file_path) {
    std::ifstream ifs(file_path.string(), std::ios::binary);
    if (!ifs.is_open()) return "";

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

    char buf[8192];
    while (ifs.read(buf, sizeof(buf)) || ifs.gcount() > 0) {
        EVP_DigestUpdate(ctx, buf, static_cast<size_t>(ifs.gcount()));
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_len; ++i)
        oss << std::setw(2) << static_cast<int>(hash[i]);
    return oss.str();
}

std::string FingerprintCache::compute_dir_fingerprint(const fs::path& dir,
                                                      const std::string& prefix) {
    std::vector<std::string> entries;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return "";

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".jpg") continue;
        std::string fname = entry.path().filename().string();
        if (!prefix.empty() && fname.find(prefix) != 0) continue;
        std::string hash = sha256_file(entry.path());
        entries.push_back(fname + ":" + hash);
    }
    std::sort(entries.begin(), entries.end());

    std::string result;
    for (const auto& e : entries) {
        if (!result.empty()) result += "\n";
        result += e;
    }
    return result;
}

void FingerprintCache::save(const fs::path& hash_file, const std::string& fingerprint) {
    std::ofstream ofs(hash_file.string());
    if (ofs.is_open()) {
        ofs << fingerprint;
    }
}

std::string FingerprintCache::load(const fs::path& hash_file) {
    if (!fs::exists(hash_file)) return "";
    std::ifstream ifs(hash_file.string());
    if (!ifs.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    return content;
}
