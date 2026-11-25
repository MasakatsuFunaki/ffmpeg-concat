#include "av/frame_grabber.h"

#include "av/av_error.h"

#include <boost/log/trivial.hpp>

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace av {
namespace {

struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

// Everything the grabber allocates, released in reverse order on any exit path.
struct Resources {
    AVFormatContext* in = nullptr;
    AVCodecContext* dec = nullptr;
    AVCodecContext* enc = nullptr;
    AVFormatContext* out = nullptr;
    SwsContext* sws = nullptr;

    ~Resources() {
        if (sws) sws_freeContext(sws);
        if (out) {
            if (out->pb && !(out->oformat->flags & AVFMT_NOFILE)) avio_closep(&out->pb);
            avformat_free_context(out);
        }
        if (enc) avcodec_free_context(&enc);
        if (dec) avcodec_free_context(&dec);
        if (in) avformat_close_input(&in);
    }
};

// Write one already-encoded still image as a single-stream container.
bool write_image(Resources& res, const fs::path& out_image, AVFrame* frame) {
    int rc = avformat_alloc_output_context2(&res.out, nullptr, nullptr,
                                            out_image.string().c_str());
    if (rc < 0 || !res.out) {
        BOOST_LOG_TRIVIAL(error) << "No image muxer for " << out_image << ": " << err_str(rc);
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder(res.out->oformat->video_codec);
    if (!codec) {
        BOOST_LOG_TRIVIAL(error) << "No encoder for " << out_image.extension();
        return false;
    }

    res.enc = avcodec_alloc_context3(codec);
    if (!res.enc) return false;

    // MJPEG only accepts the full-range J variants; everything else takes the
    // first format the encoder lists.
    AVPixelFormat target = (codec->id == AV_CODEC_ID_MJPEG) ? AV_PIX_FMT_YUVJ420P
                                                            : AV_PIX_FMT_YUV420P;
    res.enc->width     = frame->width;
    res.enc->height    = frame->height;
    res.enc->pix_fmt   = target;
    res.enc->time_base = AVRational{1, 1};
    if (codec->id == AV_CODEC_ID_MJPEG) {
        // Matches the "-q:v 2" the tool used to pass: near-visually-lossless.
        res.enc->flags |= AV_CODEC_FLAG_QSCALE;
        res.enc->global_quality = FF_QP2LAMBDA * 2;
    }

    rc = avcodec_open2(res.enc, codec, nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not open the image encoder: " << err_str(rc);
        return false;
    }

    FramePtr converted(av_frame_alloc());
    if (!converted) return false;
    converted->format = target;
    converted->width  = frame->width;
    converted->height = frame->height;
    rc = av_frame_get_buffer(converted.get(), 0);
    if (rc < 0) return false;

    res.sws = sws_getContext(frame->width, frame->height,
                             static_cast<AVPixelFormat>(frame->format),
                             frame->width, frame->height, target,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!res.sws) {
        BOOST_LOG_TRIVIAL(error) << "Could not build the pixel format conversion.";
        return false;
    }
    sws_scale(res.sws, frame->data, frame->linesize, 0, frame->height,
              converted->data, converted->linesize);
    converted->pts = 0;

    AVStream* stream = avformat_new_stream(res.out, nullptr);
    if (!stream) return false;
    rc = avcodec_parameters_from_context(stream->codecpar, res.enc);
    if (rc < 0) return false;
    stream->time_base = res.enc->time_base;

    if (!(res.out->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&res.out->pb, out_image.string().c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not create " << out_image << ": " << err_str(rc);
            return false;
        }
    }
    rc = avformat_write_header(res.out, nullptr);
    if (rc < 0) return false;

    rc = avcodec_send_frame(res.enc, converted.get());
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Image encode failed: " << err_str(rc);
        return false;
    }
    avcodec_send_frame(res.enc, nullptr);   // flush: a still is one frame

    PacketPtr pkt(av_packet_alloc());
    if (!pkt) return false;
    for (;;) {
        rc = avcodec_receive_packet(res.enc, pkt.get());
        if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) break;
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Image encode failed: " << err_str(rc);
            return false;
        }
        pkt->stream_index = stream->index;
        rc = av_interleaved_write_frame(res.out, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write the image: " << err_str(rc);
            return false;
        }
    }

    rc = av_write_trailer(res.out);
    return rc >= 0;
}

}  // namespace

bool grab_frame(const fs::path& in_path, const fs::path& out_image, double timestamp_sec) {
    try {
        Resources res;

        int rc = avformat_open_input(&res.in, in_path.string().c_str(), nullptr, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open " << in_path << ": " << err_str(rc);
            return false;
        }
        rc = avformat_find_stream_info(res.in, nullptr);
        if (rc < 0) return false;

        int index = av_find_best_stream(res.in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (index < 0) {
            BOOST_LOG_TRIVIAL(error) << in_path << " has no video stream.";
            return false;
        }
        AVStream* stream = res.in->streams[index];

        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) return false;
        res.dec = avcodec_alloc_context3(decoder);
        if (!res.dec) return false;
        rc = avcodec_parameters_to_context(res.dec, stream->codecpar);
        if (rc < 0) return false;
        res.dec->pkt_timebase = stream->time_base;
        rc = avcodec_open2(res.dec, decoder, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open the decoder: " << err_str(rc);
            return false;
        }

        // Land on the keyframe at or before the timestamp, then decode forward to
        // it — the same thing input seeking did, without the process boundary.
        int64_t target = av_rescale_q(static_cast<int64_t>(timestamp_sec * AV_TIME_BASE),
                                      AVRational{1, AV_TIME_BASE}, stream->time_base);
        rc = av_seek_frame(res.in, index, target, AVSEEK_FLAG_BACKWARD);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not seek to " << timestamp_sec
                                     << " s: " << err_str(rc);
            return false;
        }
        avcodec_flush_buffers(res.dec);

        PacketPtr pkt(av_packet_alloc());
        FramePtr frame(av_frame_alloc());
        if (!pkt || !frame) return false;

        bool found = false;
        while (!found) {
            rc = av_read_frame(res.in, pkt.get());
            if (rc == AVERROR_EOF) {
                avcodec_send_packet(res.dec, nullptr);   // drain
            } else if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Read failed: " << err_str(rc);
                return false;
            } else if (pkt->stream_index != index) {
                av_packet_unref(pkt.get());
                continue;
            } else {
                rc = avcodec_send_packet(res.dec, pkt.get());
                av_packet_unref(pkt.get());
                if (rc < 0 && rc != AVERROR(EAGAIN)) {
                    BOOST_LOG_TRIVIAL(error) << "Decode failed: " << err_str(rc);
                    return false;
                }
            }

            for (;;) {
                rc = avcodec_receive_frame(res.dec, frame.get());
                if (rc == AVERROR(EAGAIN)) break;
                if (rc == AVERROR_EOF) {
                    BOOST_LOG_TRIVIAL(error) << "No frame at " << timestamp_sec << " s.";
                    return false;
                }
                if (rc < 0) {
                    BOOST_LOG_TRIVIAL(error) << "Decode failed: " << err_str(rc);
                    return false;
                }
                if (frame->pts == AV_NOPTS_VALUE || frame->pts >= target) {
                    found = true;
                    break;
                }
                av_frame_unref(frame.get());
            }
        }

        return write_image(res, out_image, frame.get());
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Frame extraction failed: " << e.what();
        return false;
    }
}

}  // namespace av
