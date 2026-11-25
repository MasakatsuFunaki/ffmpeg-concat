#include "media/media_fs.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <boost/log/trivial.hpp>

namespace media {

std::time_t file_date(const fs::path& filepath) {
    std::string stem = filepath.stem().string();
    std::tm tm = {};
    std::istringstream ss(stem);
    ss >> std::get_time(&tm, "%Y-%m-%d %H-%M-%S");
    if (!ss.fail()) {
        return std::mktime(&tm);
    }
    // Fallback: use last write time
    auto ftime = fs::last_write_time(filepath);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sctp);
}

std::string current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
    return oss.str();
}

std::vector<fs::path> collect_video_files(const fs::path& input_dir) {
    static const std::vector<std::string> video_exts = {
        ".mp4", ".avi", ".mkv", ".mov", ".wmv", ".flv", ".webm", ".ts", ".m4v"
    };
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        for (const auto& video_ext : video_exts) {
            if (ext == video_ext) {
                files.push_back(entry.path());
                break;
            }
        }
    }
    std::sort(files.begin(), files.end(),
              [](const fs::path& a, const fs::path& b) {
                  return file_date(a) < file_date(b);
              });
    return files;
}

std::vector<fs::path> collect_mp4_files(const fs::path& input_dir) {
    std::vector<fs::path> mp4_files;
    for (const auto& entry : fs::directory_iterator(input_dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (ext == ".mp4") {
            mp4_files.push_back(entry.path());
        }
    }
    std::sort(mp4_files.begin(), mp4_files.end(),
              [](const fs::path& a, const fs::path& b) {
                  return file_date(a) < file_date(b);
              });
    return mp4_files;
}

void log_output_size(const fs::path& output_file) {
    auto size = fs::file_size(output_file);
    double size_mb = static_cast<double>(size) / (1024.0 * 1024.0);
    BOOST_LOG_TRIVIAL(info) << "Success! Output saved to: " << output_file;
    BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(2)
              << "Output file size: " << size_mb << " MB";
}

}  // namespace media
