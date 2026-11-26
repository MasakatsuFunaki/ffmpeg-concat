#include "encode/encode_plan.h"

#include "core/format_util.h"
#include "encode/encoder_catalog.h"
#include "encode/encoder_profile.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace {

// Rates that differ by no more than this tolerance are treated as uniform.
constexpr double kFpsEpsilon = 0.01;

}  // namespace

av::FrameRate lowest_fps_if_mixed(std::span<const av::FrameRate> rates) {
    auto known = rates | std::views::filter(&av::FrameRate::known);
    const auto [min_it, max_it] =
        std::ranges::minmax_element(known, {}, &av::FrameRate::value);
    if (min_it == max_it) {
        return {};
    }
    const av::FrameRate min_rate = *min_it;
    const av::FrameRate max_rate = *max_it;
    if (max_rate.value() - min_rate.value() <= kFpsEpsilon) {
        return {};
    }
    return min_rate;
}

std::string build_filter_description(const EncodePlan& plan) {
    if (plan.inputs.empty()) {
        return {};
    }

    const bool normalize = plan.target_fps.known();
    // Exact rational "N/D" avoids lossy decimal formatting and locale issues.
    const std::string rate = normalize
        ? std::format("{}/{}", plan.target_fps.num, plan.target_fps.den)
        : std::string{};

    if (plan.inputs.size() == 1) {
        const std::string video = normalize
            ? std::format("[in0v]fps={}[outv]", rate)
            : std::string{"[in0v]null[outv]"};
        if (!plan.has_audio) {
            return video;
        }
        return video + ";[in0a]anull[outa]";
    }

    const auto indices = std::views::iota(std::size_t{0}, plan.inputs.size());

    std::string filter;
    if (normalize) {
        filter = core::join(
            indices | std::views::transform([&rate](std::size_t i) {
                return std::format("[in{}v]fps={}[v{}]", i, rate, i);
            }),
            ";");
        filter.push_back(';');
    }

    const bool has_audio = plan.has_audio;
    filter += core::join(
        indices | std::views::transform([normalize, has_audio](std::size_t i) {
            if (has_audio) {
                return normalize ? std::format("[v{}][in{}a]", i, i)
                                 : std::format("[in{}v][in{}a]", i, i);
            }
            return normalize ? std::format("[v{}]", i)
                             : std::format("[in{}v]", i);
        }),
        "");

    if (has_audio) {
        filter += std::format("concat=n={}:v=1:a=1[outv][outa]", plan.inputs.size());
    } else {
        filter += std::format("concat=n={}:v=1:a=0[outv]", plan.inputs.size());
    }
    return filter;
}

av::TranscodeRequest build_request(const EncodePlan& plan) {
    const auto profile = EncoderCatalog::profile_for(plan.encoder);
    EncoderOptions encoder_options = profile->options(plan.crf, plan.preset);

    av::TranscodeRequest req;
    req.inputs = plan.inputs;
    req.output = plan.output;
    req.filter_description = build_filter_description(plan);
    req.video_encoder = profile->name();
    req.video_options = std::move(encoder_options.opts);
    req.video_bit_rate = encoder_options.bit_rate;
    req.hwaccel_cuda = plan.hwaccel_cuda;
    return req;
}
