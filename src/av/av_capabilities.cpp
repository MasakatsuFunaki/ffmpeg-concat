#include "av/av_capabilities.h"

#include "av/av_error.h"

#include <boost/log/trivial.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace av {

bool encoder_exists(const std::string& name) {
    return avcodec_find_encoder_by_name(name.c_str()) != nullptr;
}

bool can_use_encoder(const std::string& name) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) return false;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;

    // Minimal-but-valid parameters. Every encoder this project selects accepts
    // 8-bit 4:2:0, so opening tells us about the driver, not about the format.
    ctx->width     = 256;
    ctx->height    = 256;
    ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    ctx->time_base = AVRational{1, 25};
    ctx->framerate = AVRational{25, 1};

    int rc = avcodec_open2(ctx, codec, nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(debug) << "Encoder " << name << " did not open: " << err_str(rc);
    }
    avcodec_free_context(&ctx);
    return rc >= 0;
}

bool can_use_hwaccel(const std::string& type) {
    AVHWDeviceType kind = av_hwdevice_find_type_by_name(type.c_str());
    if (kind == AV_HWDEVICE_TYPE_NONE) return false;

    // Creating the device is the honest test: decoding with a hardware format
    // silently falls back to software when the driver is absent, so a trial
    // decode would report success either way.
    AVBufferRef* device = nullptr;
    int rc = av_hwdevice_ctx_create(&device, kind, nullptr, nullptr, 0);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(debug) << "Hardware device " << type
                                 << " could not be created: " << err_str(rc);
        return false;
    }
    av_buffer_unref(&device);
    return true;
}

}  // namespace av
