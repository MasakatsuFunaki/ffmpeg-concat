#include "encode/encoder_profile.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct DialectSpec {
    std::string_view quality_key;
    std::string_view preset_key;
    std::string_view default_preset;
    std::int64_t bit_rate = 0;
};

constexpr DialectSpec kNvencDialect{"cq", "preset", "p4", 0};
constexpr DialectSpec kSvtAv1Dialect{"crf", "preset", "6", 0};
constexpr DialectSpec kX264Dialect{"crf", "preset", "medium", 0};
constexpr DialectSpec kLibaomDialect{"crf", "cpu-used", "4", 0};

constexpr std::string_view kSvtAv1Name = "libsvtav1";
constexpr std::string_view kX264Name = "libx264";

[[nodiscard]] EncoderOptions make_options(const DialectSpec& spec,
                                          int crf,
                                          std::string_view preset) {
    const std::string_view resolved_preset = preset.empty() ? spec.default_preset : preset;
    return EncoderOptions{
        .opts = {
            {std::string{spec.quality_key}, std::format("{}", crf)},
            {std::string{spec.preset_key}, std::string{resolved_preset}},
        },
        .bit_rate = spec.bit_rate,
    };
}

}  // namespace

EncoderProfile::~EncoderProfile() = default;

NvencProfile::NvencProfile(std::string name) : name_(std::move(name)) {}

std::string NvencProfile::name() const {
    return name_;
}

EncoderOptions NvencProfile::options(int crf, std::string_view preset) const {
    return make_options(kNvencDialect, crf, preset);
}

std::string SvtAv1Profile::name() const {
    return std::string{kSvtAv1Name};
}

EncoderOptions SvtAv1Profile::options(int crf, std::string_view preset) const {
    return make_options(kSvtAv1Dialect, crf, preset);
}

std::string X264Profile::name() const {
    return std::string{kX264Name};
}

EncoderOptions X264Profile::options(int crf, std::string_view preset) const {
    return make_options(kX264Dialect, crf, preset);
}

LibaomProfile::LibaomProfile(std::string name) : name_(std::move(name)) {}

std::string LibaomProfile::name() const {
    return name_;
}

EncoderOptions LibaomProfile::options(int crf, std::string_view preset) const {
    return make_options(kLibaomDialect, crf, preset);
}
