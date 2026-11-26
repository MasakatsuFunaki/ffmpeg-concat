#include "av/frame_grabber.h"

#include "av/av_error.h"
#include "av/av_handles.h"
#include "core/path_util.h"
#include "core/scoped_handle.h"

#include <boost/log/trivial.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <system_error>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace av {

namespace {

constexpr int kAutoStreamIndex = -1;
constexpr int kMjpegQualityScale = 2;
constexpr int kDefaultBufferAlign = 0;
constexpr std::int64_t kStillFramePts = 0;
constexpr AVRational kStillImageTimeBase{1, 1};

// Owned libav*/libswscale resources; reverse member destruction releases
// sws -> out -> enc -> dec -> in on any exit path (Rule of Zero).
struct Resources {
    detail::FormatInputPtr in;
    detail::CodecContextPtr dec;
    detail::CodecContextPtr enc;
    detail::FormatOutputPtr out;
    detail::SwsContextPtr sws;
};

[[nodiscard]] bool write_image(Resources& res,
                               const fs::path& out_image,
                               const AVFrame* frame,
                               bool& created_output) {
    const std::string out_path_str = core::to_utf8(out_image);
    int rc = avformat_alloc_output_context2(
        core::out_ptr(res.out), nullptr, nullptr, out_path_str.c_str());
    if (rc < 0 || !res.out) {
        BOOST_LOG_TRIVIAL(error) << "No image muxer for " << out_path_str << ": " << err_str(rc);
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder(res.out->oformat->video_codec);
    if (!codec) {
        BOOST_LOG_TRIVIAL(error) << "No encoder for " << core::to_utf8(out_image.extension());
        return false;
    }

    res.enc.reset(avcodec_alloc_context3(codec));
    if (!res.enc) {
        return false;
    }

    // MJPEG requires full-range YUVJ420P; other still encoders accept YUV420P.
    const AVPixelFormat target =
        (codec->id == AV_CODEC_ID_MJPEG) ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
    res.enc->width = frame->width;
    res.enc->height = frame->height;
    res.enc->pix_fmt = target;
    res.enc->time_base = kStillImageTimeBase;
    if (codec->id == AV_CODEC_ID_MJPEG) {
        // Matches "-q:v 2" quality (near-visually-lossless JPEG).
        res.enc->flags |= AV_CODEC_FLAG_QSCALE;
        res.enc->global_quality = FF_QP2LAMBDA * kMjpegQualityScale;
    }

    rc = avcodec_open2(res.enc.get(), codec, nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not open the image encoder: " << err_str(rc);
        return false;
    }

    detail::FramePtr converted = detail::make_frame();
    if (!converted) {
        return false;
    }
    converted->format = target;
    converted->width = frame->width;
    converted->height = frame->height;
    rc = av_frame_get_buffer(converted.get(), kDefaultBufferAlign);
    if (rc < 0) {
        return false;
    }

    res.sws.reset(sws_getContext(frame->width, frame->height,
                                 static_cast<AVPixelFormat>(frame->format),
                                 frame->width, frame->height, target,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!res.sws) {
        BOOST_LOG_TRIVIAL(error) << "Could not build the pixel format conversion.";
        return false;
    }
    sws_scale(res.sws.get(), frame->data, frame->linesize, 0, frame->height,
              converted->data, converted->linesize);
    converted->pts = kStillFramePts;

    AVStream* stream = avformat_new_stream(res.out.get(), nullptr);
    if (!stream) {
        return false;
    }
    rc = avcodec_parameters_from_context(stream->codecpar, res.enc.get());
    if (rc < 0) {
        return false;
    }
    stream->time_base = res.enc->time_base;

    if (!(res.out->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&res.out->pb, out_path_str.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not create " << out_path_str << ": " << err_str(rc);
            return false;
        }
        created_output = true;
    }
    rc = avformat_write_header(res.out.get(), nullptr);
    if (rc < 0) {
        return false;
    }

    rc = avcodec_send_frame(res.enc.get(), converted.get());
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Image encode failed: " << err_str(rc);
        return false;
    }
    avcodec_send_frame(res.enc.get(), nullptr);  // flush single still frame

    detail::PacketPtr pkt = detail::make_packet();
    if (!pkt) {
        return false;
    }
    for (;;) {
        rc = avcodec_receive_packet(res.enc.get(), pkt.get());
        if (rc == AVERROR_EOF || rc == AVERROR(EAGAIN)) {
            break;
        }
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Image encode failed: " << err_str(rc);
            return false;
        }
        pkt->stream_index = stream->index;
        rc = av_interleaved_write_frame(res.out.get(), pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write the image: " << err_str(rc);
            return false;
        }
    }

    return av_write_trailer(res.out.get()) >= 0;
}

}  // namespace

bool grab_frame(const fs::path& in_path, const fs::path& out_image, double timestamp_sec) {
    try {
        bool created_output = false;
        auto remove_partial_output = core::make_scope_guard([&out_image, &created_output]() noexcept {
            if (created_output) {
                std::error_code ec;
                fs::remove(out_image, ec);
            }
        });

        Resources res;

        const std::string in_path_str = core::to_utf8(in_path);
        int rc = avformat_open_input(core::out_ptr(res.in), in_path_str.c_str(), nullptr, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open " << in_path_str << ": " << err_str(rc);
            return false;
        }
        rc = avformat_find_stream_info(res.in.get(), nullptr);
        if (rc < 0) {
            return false;
        }

        const int index = av_find_best_stream(
            res.in.get(), AVMEDIA_TYPE_VIDEO, kAutoStreamIndex, kAutoStreamIndex, nullptr, 0);
        if (index < 0) {
            BOOST_LOG_TRIVIAL(error) << in_path_str << " has no video stream.";
            return false;
        }
        const AVStream* stream = res.in->streams[index];

        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            return false;
        }
        res.dec.reset(avcodec_alloc_context3(decoder));
        if (!res.dec) {
            return false;
        }
        rc = avcodec_parameters_to_context(res.dec.get(), stream->codecpar);
        if (rc < 0) {
            return false;
        }
        res.dec->pkt_timebase = stream->time_base;
        rc = avcodec_open2(res.dec.get(), decoder, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open the decoder: " << err_str(rc);
            return false;
        }

        // Land on the keyframe at or before timestamp_sec, then decode forward.
        const std::int64_t target = av_rescale_q(
            static_cast<std::int64_t>(timestamp_sec * AV_TIME_BASE),
            AVRational{1, AV_TIME_BASE},
            stream->time_base);
        rc = av_seek_frame(res.in.get(), index, target, AVSEEK_FLAG_BACKWARD);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not seek to " << timestamp_sec
                                     << " s: " << err_str(rc);
            return false;
        }
        avcodec_flush_buffers(res.dec.get());

        detail::PacketPtr pkt = detail::make_packet();
        detail::FramePtr frame = detail::make_frame();
        if (!pkt || !frame) {
            return false;
        }

        bool found = false;
        while (!found) {
            rc = av_read_frame(res.in.get(), pkt.get());
            if (rc < 0 && rc != AVERROR_EOF) {
                BOOST_LOG_TRIVIAL(error) << "Read failed: " << err_str(rc);
                return false;
            }
            if (rc == AVERROR_EOF) {
                avcodec_send_packet(res.dec.get(), nullptr);  // drain
            } else {
                if (pkt->stream_index != index) {
                    av_packet_unref(pkt.get());
                    continue;
                }
                rc = avcodec_send_packet(res.dec.get(), pkt.get());
                av_packet_unref(pkt.get());
                if (rc < 0 && rc != AVERROR(EAGAIN)) {
                    BOOST_LOG_TRIVIAL(error) << "Decode failed: " << err_str(rc);
                    return false;
                }
            }

            for (;;) {
                rc = avcodec_receive_frame(res.dec.get(), frame.get());
                if (rc == AVERROR(EAGAIN)) {
                    break;
                }
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

        const bool ok = write_image(res, out_image, frame.get(), created_output);
        if (ok) {
            remove_partial_output.dismiss();
        }
        return ok;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Frame extraction failed: " << e.what();
        return false;
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Frame extraction failed with an unknown error.";
        return false;
    }
}

}  // namespace av
