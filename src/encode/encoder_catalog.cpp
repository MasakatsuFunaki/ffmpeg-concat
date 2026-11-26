#include "encode/encoder_catalog.h"

#include "av/av_capabilities.h"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr std::array<std::string_view, 2> kH264Candidates{"h264_nvenc", "libx264"};
constexpr std::array<std::string_view, 3> kAv1Candidates{"av1_nvenc", "libsvtav1", "libaom-av1"};

[[nodiscard]] std::span<const std::string_view> candidates_for(std::string_view codec) noexcept {
    if (codec == "h264") {
        return kH264Candidates;
    }
    return kAv1Candidates;
}

}  // namespace

std::string EncoderCatalog::detect(std::string_view codec) {
    const auto candidates = candidates_for(codec);
    const auto it = std::ranges::find_if(candidates, [](std::string_view enc) {
        if (!av::encoder_exists(enc)) {
            return false;
        }
        if (av::can_use_encoder(enc)) {
            return true;
        }
        BOOST_LOG_TRIVIAL(warning)
            << "Encoder '" << enc
            << "' is linked in but not usable on this host; trying next candidate.";
        return false;
    });
    return it != candidates.end() ? std::string{*it} : std::string{};
}

std::unique_ptr<EncoderProfile> EncoderCatalog::profile_for(std::string_view encoder) {
    if (encoder == "av1_nvenc" || encoder == "h264_nvenc") {
        return std::make_unique<NvencProfile>(std::string{encoder});
    }
    if (encoder == "libsvtav1") {
        return std::make_unique<SvtAv1Profile>();
    }
    if (encoder == "libx264") {
        return std::make_unique<X264Profile>();
    }
    return std::make_unique<LibaomProfile>(std::string{encoder});
}
