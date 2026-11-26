#pragma once

// In-process decode -> filter -> encode -> mux pipeline.
//
// TranscodeRequest is deliberately declarative and libav-free; policy choices
// (encoder selection, filter graphs, private options) live in the encode/ layer.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

class ProgressReporter;

namespace av {

struct TranscodeRequest {
    static constexpr std::int64_t kDefaultAudioBitRate = 192000;

    std::vector<fs::path> inputs;  // joined in this order
    fs::path output;

    // Filter graph in ffmpeg textual syntax. Unconnected input pads must be
    // named "inNv"/"inNa" for input N, and outputs "outv" (plus optional
    // "outa" when audio is routed through the graph). Omitting "[outa]" with
    // audio_copy == false produces a video-only output.
    std::string filter_description;

    std::string video_encoder;                                       // e.g. "av1_nvenc"
    std::vector<std::pair<std::string, std::string>> video_options;  // encoder private options
    std::int64_t video_bit_rate = 0;                                 // 0 => quality-targeted

    std::string audio_encoder = "aac";
    std::int64_t audio_bit_rate = kDefaultAudioBitRate;

    // Copy audio packets untouched instead of routing them through the filter
    // graph. Single-input only; the graph must have no "outa" pad.
    bool audio_copy = false;

    bool hwaccel_cuda = false;  // decode on GPU; enables NVDEC->NVENC zero-copy for passthrough video

    [[nodiscard]] bool operator==(const TranscodeRequest&) const = default;
};

// Runs the pipeline to completion. Reports progress against total_sec when > 0.
// Returns true only when the output was written and its trailer flushed.
// Never throws; failures are logged and reported as false.
[[nodiscard]] bool transcode(const TranscodeRequest& req,
                             ProgressReporter& reporter,
                             double total_sec);

}  // namespace av
