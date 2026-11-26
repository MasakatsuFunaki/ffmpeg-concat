#pragma once

// ---------------------------------------------------------------------------
// av/av_handles.h – internal RAII handle aliases for libav* / libswscale.
//
// INTERNAL HEADER: include ONLY from src/av/*.cpp so libav* headers remain
// physically insulated behind the av/ public headers.
// ---------------------------------------------------------------------------

#include "core/scoped_handle.h"

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace av::detail {

struct FormatOutputDeleter {
    void operator()(AVFormatContext* fmt) const noexcept {
        if (!fmt) {
            return;
        }
        if (fmt->pb && fmt->oformat && !(fmt->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmt->pb);
        }
        avformat_free_context(fmt);
    }
};

using FormatInputPtr  = core::UniqueHandle<AVFormatContext, avformat_close_input>;
using FormatOutputPtr = std::unique_ptr<AVFormatContext, FormatOutputDeleter>;
using CodecContextPtr = core::UniqueHandle<AVCodecContext, avcodec_free_context>;
using FramePtr        = core::UniqueHandle<AVFrame, av_frame_free>;
using PacketPtr       = core::UniqueHandle<AVPacket, av_packet_free>;
using BufferRefPtr    = core::UniqueHandle<AVBufferRef, av_buffer_unref>;
using FilterGraphPtr  = core::UniqueHandle<AVFilterGraph, avfilter_graph_free>;
using FilterInOutPtr  = core::UniqueHandle<AVFilterInOut, avfilter_inout_free>;
using DictionaryPtr   = core::UniqueHandle<AVDictionary, av_dict_free>;
using SwsContextPtr   = core::UniqueHandle<SwsContext, sws_freeContext>;

[[nodiscard]] inline FramePtr make_frame() noexcept {
    return FramePtr{av_frame_alloc()};
}

[[nodiscard]] inline PacketPtr make_packet() noexcept {
    return PacketPtr{av_packet_alloc()};
}

}  // namespace av::detail
