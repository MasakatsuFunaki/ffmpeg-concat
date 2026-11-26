#pragma once

// EncodePlan – fully-resolved description of a transcode run and the pure
// functions that assemble an av::TranscodeRequest without touching libav*.

#include "av/media_probe.h"
#include "av/transcoder.h"

#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct EncodePlan {
    std::vector<fs::path> inputs;   // concat order; more than one -> concat filter
    fs::path output;
    std::string encoder;            // resolved video encoder name
    int crf = 30;
    std::string preset;             // "" -> EncoderProfile default
    bool hwaccel_cuda = false;      // decode on GPU where supported
    av::FrameRate target_fps;       // known() -> fps=N/D before concat

    [[nodiscard]] bool operator==(const EncodePlan&) const = default;
};

// Assemble the ffmpeg filter graph for `plan` with input pads "inNv"/"inNa"
// and outputs "outv"/"outa". Returns empty string when plan.inputs is empty.
[[nodiscard]] std::string build_filter_description(const EncodePlan& plan);

// Assemble the complete av::TranscodeRequest for `plan`.
[[nodiscard]] av::TranscodeRequest build_request(const EncodePlan& plan);

// Return the lowest known frame rate if the known rates differ by more than
// the tolerance; otherwise return an unknown FrameRate (no resampling needed).
[[nodiscard]] av::FrameRate lowest_fps_if_mixed(std::span<const av::FrameRate> rates);

[[nodiscard]] inline av::FrameRate lowest_fps_if_mixed(
    std::initializer_list<av::FrameRate> rates) {
    return lowest_fps_if_mixed(std::span<const av::FrameRate>{rates.begin(), rates.size()});
}
