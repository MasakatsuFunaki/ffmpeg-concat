#pragma once

// ProgressParser::Snapshot holds one sampled view of an in-process run's progress.

#include <string>

class ProgressParser {
public:
    struct Snapshot {
        double current_sec = 0.0;
        std::string out_time;
        std::string fps;
        std::string speed;

        [[nodiscard]] bool operator==(const Snapshot&) const = default;
    };
};
