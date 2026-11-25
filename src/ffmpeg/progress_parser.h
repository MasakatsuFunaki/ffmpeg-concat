#pragma once

// ---------------------------------------------------------------------------
// ProgressParser::Snapshot – one sampled view of a run's progress.
//
// The name is historical: this used to parse ffmpeg's `-progress pipe:1` output
// line by line. Now that the pipeline runs in-process the numbers are measured
// directly, and only the value type the reporters render survived.
// ---------------------------------------------------------------------------

#include <string>

class ProgressParser {
public:
    struct Snapshot {
        double current_sec;             // output position reached so far
        std::string out_time, fps, speed;   // preformatted for display
    };
};
