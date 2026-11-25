#pragma once

// ---------------------------------------------------------------------------
// av::transcode – the decode -> filter -> encode -> mux pipeline, in-process.
//
// This is what replaced spawning `ffmpeg`: libavformat/libavcodec/libavfilter
// are linked straight into the tool, so there is no child process, no argv to
// quote, and no binary to ship alongside the executable.
//
// The request is deliberately declarative and libav-free. Deciding WHAT to run
// (which encoder, which filter chain, which private options) belongs to the
// encode/ layer; this component only executes it.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

class ProgressReporter;

namespace av {

struct TranscodeRequest {
    std::vector<fs::path> inputs;   // joined in this order
    fs::path output;

    // Filter graph in ffmpeg's textual syntax. Its unconnected input pads must be
    // named "inNv"/"inNa" for input N, and its outputs "outv"/"outa".
    std::string filter_description;

    std::string video_encoder;                                    // e.g. "av1_nvenc"
    std::vector<std::pair<std::string, std::string>> video_options;  // encoder private options
    int64_t video_bit_rate = 0;                                   // 0 => quality-targeted

    std::string audio_encoder = "aac";
    int64_t audio_bit_rate = 192000;

    // Copy the audio packets through untouched instead of routing them through
    // the graph. Single-input only, and the graph must then have no "outa" pad.
    // A source with no audio stream simply produces none.
    bool audio_copy = false;

    bool hwaccel_cuda = false;      // decode on the GPU where the codec allows it
};

// Run the pipeline to completion. Progress is reported against total_sec when it
// is > 0. Returns true only if the output was written and the trailer flushed.
// Never throws; failures are logged and reported as false.
bool transcode(const TranscodeRequest& req, ProgressReporter& reporter, double total_sec);

}  // namespace av
