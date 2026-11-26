#pragma once

// Host capability probes: distinguish encoders/hwaccels compiled into libav*
// from those whose runtime driver can actually open a session on this machine.

#include <string_view>

namespace av {

// True if an encoder by this name is compiled into the linked libavcodec.
[[nodiscard]] bool encoder_exists(std::string_view name) noexcept;

// True if an encoder by this name is compiled in and opens a trial context on
// this host.
[[nodiscard]] bool can_use_encoder(std::string_view name) noexcept;

// True if a hardware device of this type (e.g. "cuda") can be created on this
// host.
[[nodiscard]] bool can_use_hwaccel(std::string_view type) noexcept;

}  // namespace av
