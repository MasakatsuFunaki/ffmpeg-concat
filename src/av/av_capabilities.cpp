#include "av/av_capabilities.h"

#include "av/av_error.h"
#include "av/av_handles.h"
#include "core/scoped_handle.h"

#include <boost/log/trivial.hpp>

#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace av {

namespace {

constexpr int kProbeWidth = 256;
constexpr int kProbeHeight = 256;
constexpr int kProbeFps = 25;
constexpr AVRational kProbeTimeBase{1, kProbeFps};
constexpr AVRational kProbeFrameRate{kProbeFps, 1};

}  // namespace

bool encoder_exists(std::string_view name) noexcept {
    try {
        if (name.empty()) {
            return false;
        }
        const std::string codec_name{name};
        return avcodec_find_encoder_by_name(codec_name.c_str()) != nullptr;
    } catch (...) {
        return false;
    }
}

bool can_use_encoder(std::string_view name) noexcept {
    try {
        if (name.empty()) {
            return false;
        }
        const std::string codec_name{name};
        const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
        if (!codec) {
            return false;
        }

        detail::CodecContextPtr ctx{avcodec_alloc_context3(codec)};
        if (!ctx) {
            return false;
        }

        // Minimal 8-bit 4:2:0 parameters accepted by every encoder this suite
        // selects, so open failure reflects driver availability rather than format.
        ctx->width = kProbeWidth;
        ctx->height = kProbeHeight;
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->time_base = kProbeTimeBase;
        ctx->framerate = kProbeFrameRate;

        const int rc = avcodec_open2(ctx.get(), codec, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(debug) << "Encoder " << name << " did not open: " << err_str(rc);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool can_use_hwaccel(std::string_view type) noexcept {
    try {
        if (type.empty()) {
            return false;
        }
        const std::string type_name{type};
        const AVHWDeviceType kind = av_hwdevice_find_type_by_name(type_name.c_str());
        if (kind == AV_HWDEVICE_TYPE_NONE) {
            return false;
        }

        // Device creation is the authoritative check: a trial decode silently
        // falls back to software when the GPU driver is absent.
        detail::BufferRefPtr device;
        const int rc = av_hwdevice_ctx_create(core::out_ptr(device), kind, nullptr, nullptr, 0);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(debug) << "Hardware device " << type
                                     << " could not be created: " << err_str(rc);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace av
