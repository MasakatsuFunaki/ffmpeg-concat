#include "encode/encoder_profile.h"

#include <string>
#include <utility>

// NvencProfile: cq=<crf>, preset=<preset|p4>, bit_rate 0
NvencProfile::NvencProfile(std::string name) : name_(std::move(name)) {}

std::string NvencProfile::name() const { return name_; }

EncoderOptions NvencProfile::options(int crf, const std::string& preset) const {
    return {{{"cq", std::to_string(crf)},
             {"preset", preset.empty() ? "p4" : preset}},
            0};
}

// SvtAv1Profile: crf=<crf>, preset=<preset|6>
std::string SvtAv1Profile::name() const { return "libsvtav1"; }

EncoderOptions SvtAv1Profile::options(int crf, const std::string& preset) const {
    return {{{"crf", std::to_string(crf)},
             {"preset", preset.empty() ? "6" : preset}},
            0};
}

// X264Profile: crf=<crf>, preset=<preset|medium>
std::string X264Profile::name() const { return "libx264"; }

EncoderOptions X264Profile::options(int crf, const std::string& preset) const {
    return {{{"crf", std::to_string(crf)},
             {"preset", preset.empty() ? "medium" : preset}},
            0};
}

// LibaomProfile: crf=<crf>, cpu-used=<preset|4>, bit_rate 0
LibaomProfile::LibaomProfile(std::string name) : name_(std::move(name)) {}

std::string LibaomProfile::name() const { return name_; }

EncoderOptions LibaomProfile::options(int crf, const std::string& preset) const {
    return {{{"crf", std::to_string(crf)},
             {"cpu-used", preset.empty() ? "4" : preset}},
            0};
}
