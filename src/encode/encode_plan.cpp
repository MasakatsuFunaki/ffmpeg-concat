#include "encode/encode_plan.h"

#include "encode/encoder_catalog.h"
#include "encode/encoder_profile.h"

#include <algorithm>
#include <sstream>

namespace {

// Rates that differ by less than this are the same rate seen through different
// rounding, not a mix worth resampling.
constexpr double FPS_EPSILON = 0.01;

}  // namespace

av::FrameRate lowest_fps_if_mixed(const std::vector<av::FrameRate>& rates) {
    std::vector<av::FrameRate> known;
    for (const auto& r : rates) {
        if (r.known()) known.push_back(r);
    }
    if (known.size() < 2) return {};

    auto bounds = std::minmax_element(
        known.begin(), known.end(),
        [](const av::FrameRate& a, const av::FrameRate& b) { return a.value() < b.value(); });
    if (bounds.second->value() - bounds.first->value() <= FPS_EPSILON) return {};  // uniform
    return *bounds.first;
}

std::string build_filter_description(const EncodePlan& plan) {
    const bool normalize = plan.target_fps.known();
    // Emitted as an exact rational: "30/1", never "30.000000". libavfilter parses
    // numbers through the C locale, and any library in the process (indicators
    // among them) can switch that to comma-decimal mid-run.
    const std::string rate = normalize ? std::to_string(plan.target_fps.num) + "/" +
                                             std::to_string(plan.target_fps.den)
                                       : "";
    std::ostringstream filter;

    if (plan.inputs.size() < 2) {
        // Nothing to join: pass the streams straight through, applying the rate
        // change only if one was asked for.
        filter << "[in0v]" << (normalize ? "fps=" + rate : "null") << "[outv];"
               << "[in0a]anull[outa]";
        return filter.str();
    }

    // The concat filter does no rate conversion, so segments recorded at
    // different rates would join into a variable-frame-rate output. Normalize
    // each input ahead of the concat instead, so every segment enters it at the
    // same rate.
    if (normalize) {
        for (size_t i = 0; i < plan.inputs.size(); ++i) {
            filter << "[in" << i << "v]fps=" << rate << "[v" << i << "];";
        }
    }
    for (size_t i = 0; i < plan.inputs.size(); ++i) {
        if (normalize) {
            filter << "[v" << i << "]";
        } else {
            filter << "[in" << i << "v]";
        }
        filter << "[in" << i << "a]";
    }
    filter << "concat=n=" << plan.inputs.size() << ":v=1:a=1[outv][outa]";
    return filter.str();
}

av::TranscodeRequest build_request(const EncodePlan& plan) {
    auto profile = EncoderCatalog::profile_for(plan.encoder);
    EncoderOptions encoder_options = profile->options(plan.crf, plan.preset);

    av::TranscodeRequest req;
    req.inputs             = plan.inputs;
    req.output             = plan.output;
    req.filter_description = build_filter_description(plan);
    req.video_encoder      = profile->name();
    req.video_options      = std::move(encoder_options.opts);
    req.video_bit_rate     = encoder_options.bit_rate;
    req.hwaccel_cuda       = plan.hwaccel_cuda;
    return req;
}
