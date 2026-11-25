#pragma once

// ---------------------------------------------------------------------------
// EncodePlan – the fully-resolved description of one transcode run, and the
// pure functions that turn it into something the av/ engine can execute.
//
// Every runtime decision (which encoder, whether CUDA decoding works on this
// host, what rate mismatched segments must be normalized to) is taken by the
// caller and recorded here; the functions below then only assemble strings and
// options. Keeping that assembly free of libav* calls is what lets the exact
// filter graph and encoder options be asserted in unit tests, with no codec
// opened and no GPU present.
// ---------------------------------------------------------------------------

#include "av/media_probe.h"
#include "av/transcoder.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct EncodePlan {
    std::vector<fs::path> inputs;   // concat order; more than one -> concat filter
    fs::path output;
    std::string encoder;            // resolved video encoder name
    int crf = 30;
    std::string preset;             // "" -> the EncoderProfile's own default
    bool hwaccel_cuda = false;      // decode on the GPU where the codec allows it
    av::FrameRate target_fps;       // known() -> fps=N on every input before concat
};

// The filter graph for `plan`, in ffmpeg's textual syntax. Input pads are named
// "inNv"/"inNa" per input N and the outputs "outv"/"outa", which is the contract
// av::transcode() expects.
std::string build_filter_description(const EncodePlan& plan);

// Everything the engine needs to run `plan`.
av::TranscodeRequest build_request(const EncodePlan& plan);

// The rate every segment must be normalized to, or an unknown() rate when no
// normalization is needed. Unknown entries are unprobed and ignored. Returns the
// lowest known rate iff the known rates actually differ; with fewer than two
// known rates, or with rates that already agree, returns an unknown rate so no
// fps filter is emitted.
av::FrameRate lowest_fps_if_mixed(const std::vector<av::FrameRate>& rates);
