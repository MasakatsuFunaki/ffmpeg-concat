#pragma once

// ---------------------------------------------------------------------------
// EncoderProfile – per-encoder option dialect (GoF "Strategy" pattern).
//
// Every encoder spells the same two ideas differently: NVENC calls constant
// quality "cq" and wants an explicit zero bitrate, x264 and SVT-AV1 call it
// "crf", and libaom calls its speed preset "cpu-used". Each concrete profile
// knows the name libavcodec registers (name()) and the private options that
// encoder expects (options()).
//
// Returning plain key/value pairs rather than an AVDictionary keeps this header
// free of libav* types, and keeps the dialect unit-testable without opening a
// codec. EncoderCatalog::profile_for() is the Factory Method that hands back the
// right profile for a resolved encoder name.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct EncoderOptions {
    // Private options applied to the encoder before it is opened.
    std::vector<std::pair<std::string, std::string>> opts;
    // Target bitrate in bits/s. 0 means quality-targeted (the old "-b:v 0").
    int64_t bit_rate = 0;
};

class EncoderProfile {
public:
    virtual ~EncoderProfile() = default;
    // The encoder name looked up with avcodec_find_encoder_by_name().
    virtual std::string name() const = 0;
    // This encoder's quality/speed dialect for the requested crf and preset.
    virtual EncoderOptions options(int crf, const std::string& preset) const = 0;
};

// NvencProfile(name): cq=<crf>, preset=<preset|p4>, bit_rate 0  (av1_nvenc / h264_nvenc)
class NvencProfile : public EncoderProfile {
public:
    explicit NvencProfile(std::string name);
    std::string name() const override;
    EncoderOptions options(int crf, const std::string& preset) const override;

private:
    std::string name_;
};

// SvtAv1Profile: crf=<crf>, preset=<preset|6>   name "libsvtav1"
class SvtAv1Profile : public EncoderProfile {
public:
    std::string name() const override;
    EncoderOptions options(int crf, const std::string& preset) const override;
};

// X264Profile: crf=<crf>, preset=<preset|medium>   name "libx264"
class X264Profile : public EncoderProfile {
public:
    std::string name() const override;
    EncoderOptions options(int crf, const std::string& preset) const override;
};

// LibaomProfile(name): crf=<crf>, cpu-used=<preset|4>, bit_rate 0.
// Fallthrough for libaom-av1 AND any unrecognized forced encoder name.
class LibaomProfile : public EncoderProfile {
public:
    explicit LibaomProfile(std::string name);
    std::string name() const override;
    EncoderOptions options(int crf, const std::string& preset) const override;

private:
    std::string name_;
};
