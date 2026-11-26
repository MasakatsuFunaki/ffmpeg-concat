#pragma once

// EncoderProfile – per-encoder option dialect (Strategy pattern).
// Returns plain key/value pairs so option assembly stays insulated from libav*
// types and directly unit-testable without opening a codec.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct EncoderOptions {
    // Private options applied to the encoder before it is opened.
    std::vector<std::pair<std::string, std::string>> opts;
    // Target bitrate in bits/s. 0 selects quality-targeted encoding.
    std::int64_t bit_rate = 0;

    [[nodiscard]] bool operator==(const EncoderOptions&) const = default;
};

class EncoderProfile {
public:
    virtual ~EncoderProfile();

    // Encoder name looked up via avcodec_find_encoder_by_name().
    [[nodiscard]] virtual std::string name() const = 0;

    // Quality/speed option dialect for the requested crf and preset.
    [[nodiscard]] virtual EncoderOptions options(int crf, std::string_view preset) const = 0;

protected:
    EncoderProfile() = default;
    EncoderProfile(const EncoderProfile&) = default;
    EncoderProfile& operator=(const EncoderProfile&) = default;
    EncoderProfile(EncoderProfile&&) noexcept = default;
    EncoderProfile& operator=(EncoderProfile&&) noexcept = default;
};

// NvencProfile(name): cq=<crf>, preset=<preset|p4>, bit_rate 0  (av1_nvenc / h264_nvenc)
class NvencProfile final : public EncoderProfile {
public:
    explicit NvencProfile(std::string name);
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] EncoderOptions options(int crf, std::string_view preset) const override;

private:
    std::string name_;
};

// SvtAv1Profile: crf=<crf>, preset=<preset|6>, name "libsvtav1"
class SvtAv1Profile final : public EncoderProfile {
public:
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] EncoderOptions options(int crf, std::string_view preset) const override;
};

// X264Profile: crf=<crf>, preset=<preset|medium>, name "libx264"
class X264Profile final : public EncoderProfile {
public:
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] EncoderOptions options(int crf, std::string_view preset) const override;
};

// LibaomProfile(name): crf=<crf>, cpu-used=<preset|4>, bit_rate 0.
// Fallthrough for libaom-av1 and any unrecognized forced encoder name.
class LibaomProfile final : public EncoderProfile {
public:
    explicit LibaomProfile(std::string name);
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] EncoderOptions options(int crf, std::string_view preset) const override;

private:
    std::string name_;
};
