#pragma once

// ---------------------------------------------------------------------------
// EncoderCatalog – encoder discovery + profile selection (GoF "Factory Method").
//
//   detect()      returns the highest-priority encoder for the requested codec
//                 that is both compiled in AND actually usable here ("" if none).
//   profile_for() maps a resolved encoder name to its EncoderProfile Strategy.
// ---------------------------------------------------------------------------

#include "encode/encoder_profile.h"

#include <memory>
#include <string>

class EncoderCatalog {
public:
    // Priority: codec=="h264" -> {h264_nvenc, libx264}, else -> {av1_nvenc,
    // libsvtav1, libaom-av1}. Being compiled in is only a pre-filter; each
    // surviving candidate is then opened for real (av::can_use_encoder) so a
    // present-but-unusable encoder (e.g. av1_nvenc with no NVIDIA driver) is
    // skipped. Returns the first usable candidate, or "" if none are.
    static std::string detect(const std::string& codec);

    // av1_nvenc|h264_nvenc -> NvencProfile(name); libsvtav1 -> SvtAv1Profile;
    // libx264 -> X264Profile; anything else -> LibaomProfile(name).
    static std::unique_ptr<EncoderProfile> profile_for(const std::string& encoder);
};
