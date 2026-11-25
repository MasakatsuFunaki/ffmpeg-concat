#include "encode/encoder_catalog.h"

#include "av/av_capabilities.h"

#include <boost/log/trivial.hpp>

#include <vector>

std::string EncoderCatalog::detect(const std::string& codec) {
    std::vector<std::string> candidates;
    if (codec == "h264") {
        candidates = { "h264_nvenc", "libx264" };
    } else {
        candidates = { "av1_nvenc", "libsvtav1", "libaom-av1" };
    }

    // Being linked in is necessary but not sufficient: the NVENC wrappers are
    // always present and only fail when a session is opened against a missing
    // driver. Confirm each candidate really opens here, preserving the
    // hardware-first priority order.
    for (const auto& enc : candidates) {
        if (!av::encoder_exists(enc)) {
            continue;  // not built into this libavcodec
        }
        if (av::can_use_encoder(enc)) {
            return enc;
        }
        BOOST_LOG_TRIVIAL(warning) << "Encoder '" << enc
            << "' is linked in but not usable on this host; trying next candidate.";
    }

    return "";
}

std::unique_ptr<EncoderProfile> EncoderCatalog::profile_for(const std::string& encoder) {
    if (encoder == "av1_nvenc" || encoder == "h264_nvenc") {
        return std::make_unique<NvencProfile>(encoder);
    } else if (encoder == "libsvtav1") {
        return std::make_unique<SvtAv1Profile>();
    } else if (encoder == "libx264") {
        return std::make_unique<X264Profile>();
    } else {
        return std::make_unique<LibaomProfile>(encoder);
    }
}
