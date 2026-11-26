#pragma once

// EncoderCatalog – encoder discovery and EncoderProfile factory.
//   detect()      returns the highest-priority compiled-in and usable encoder
//                 for the requested codec (empty string if none).
//   profile_for() maps a resolved encoder name to its EncoderProfile Strategy.

#include "encode/encoder_profile.h"

#include <memory>
#include <string>
#include <string_view>

class EncoderCatalog {
public:
    // Priority: codec == "h264" -> {h264_nvenc, libx264},
    // otherwise -> {av1_nvenc, libsvtav1, libaom-av1}.
    // Each compiled-in candidate is verified via av::can_use_encoder so
    // hardware encoders without a host driver are skipped.
    [[nodiscard]] static std::string detect(std::string_view codec);

    // av1_nvenc|h264_nvenc -> NvencProfile(name); libsvtav1 -> SvtAv1Profile;
    // libx264 -> X264Profile; anything else -> LibaomProfile(name).
    [[nodiscard]] static std::unique_ptr<EncoderProfile> profile_for(std::string_view encoder);
};
