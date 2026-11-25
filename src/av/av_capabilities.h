#pragma once

// ---------------------------------------------------------------------------
// av_capabilities – what this build, on this host, can actually do.
//
// "Compiled in" and "works here" are different questions: the NVENC wrappers
// are always compiled in, and only fail when the encoder session is opened
// against a missing driver. Each query below answers the second question by
// really constructing the thing.
// ---------------------------------------------------------------------------

#include <string>

namespace av {

// True if an encoder by this name is compiled into the linked libavcodec.
bool encoder_exists(const std::string& name);

// True if an encoder by this name also OPENS on this host — the in-process
// equivalent of a trial encode. Never throws.
bool can_use_encoder(const std::string& name);

// True if a hardware device of this type ("cuda", ...) can be created here.
// Never throws.
bool can_use_hwaccel(const std::string& type);

}  // namespace av
