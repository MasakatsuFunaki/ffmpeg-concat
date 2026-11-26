#include "av/frame_sampler.h"

#include "av/av_error.h"
#include "av/av_handles.h"
#include "av/bgr_converter.h"
#include "core/path_util.h"
#include "core/profiler.h"
#include "core/scoped_handle.h"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace av {

namespace {

constexpr int kNoStreamIndex = -1;
constexpr int kAutoStreamIndex = -1;
constexpr int kAutoThreadCount = 0;
constexpr int kFirstRowIndex = 0;
constexpr std::size_t kSwsPlaneCount = 4;
constexpr double kMinTimestampSec = 0.0;
constexpr double kMinSeekSkipSec = 0.60;
constexpr double kUnindexedForwardAdvanceSec = 0.75;
constexpr double kEofMarginSec = 0.5;
constexpr double kMaxRescaleSec = 1.0e12;
constexpr AVRational kAvTimeBaseRational{1, AV_TIME_BASE};

[[nodiscard]] std::int64_t extract_frame_pts(const AVFrame* frame) noexcept {
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        return frame->best_effort_timestamp;
    }
    return frame->pts;
}

[[nodiscard]] double compute_duration_sec(const AVFormatContext* fmt,
                                          const AVStream* stream) noexcept {
    double dur = 0.0;
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        dur = static_cast<double>(fmt->duration) / static_cast<double>(AV_TIME_BASE);
    } else if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0 &&
               stream->time_base.num > 0 && stream->time_base.den > 0) {
        dur = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }
    return (std::isfinite(dur) && dur > 0.0) ? dur : 0.0;
}

void log_error_nothrow(const char* prefix, const char* detail) noexcept {
    try {
        BOOST_LOG_TRIVIAL(error) << prefix << detail;
    } catch (...) {
    }
}

}  // namespace

struct FrameSampler::Impl {
    // Member declaration order enforces reverse RAII destruction:
    // sws -> last_frame -> pkt -> dec -> in.
    detail::FormatInputPtr in;
    detail::CodecContextPtr dec;
    detail::PacketPtr pkt;
    detail::FramePtr last_frame;
    detail::SwsContextPtr sws;

    int v_index = kNoStreamIndex;
    AVRational time_base{0, 1};
    double duration_sec = 0.0;
    int width = 0;
    int height = 0;

    int sws_width = 0;
    int sws_height = 0;
    AVPixelFormat sws_fmt = AV_PIX_FMT_NONE;
    AVPixelFormat sws_dst_fmt = AV_PIX_FMT_NONE;
    BgrConverter<> bgr_converter;

    // Decoder position invariant: whenever has_last_frame && last_pts != AV_NOPTS_VALUE,
    // last_frame is the first video frame with pts >= last_requested_target, so no frame
    // exists in [last_requested_target, last_pts).
    bool has_last_frame = false;
    std::int64_t last_pts = AV_NOPTS_VALUE;
    std::int64_t last_requested_target = AV_NOPTS_VALUE;
    bool demux_eof = false;
    bool eof_reached = false;

    std::string path_utf8;

    [[nodiscard]] bool is_open() const noexcept {
        return in && dec && pkt && last_frame && v_index >= 0;
    }

    void reset() noexcept {
        sws.reset();
        last_frame.reset();
        pkt.reset();
        dec.reset();
        in.reset();

        v_index = kNoStreamIndex;
        time_base = AVRational{0, 1};
        duration_sec = 0.0;
        width = 0;
        height = 0;

        sws_width = 0;
        sws_height = 0;
        sws_fmt = AV_PIX_FMT_NONE;
        sws_dst_fmt = AV_PIX_FMT_NONE;
        bgr_converter.clear();

        has_last_frame = false;
        last_pts = AV_NOPTS_VALUE;
        last_requested_target = AV_NOPTS_VALUE;
        demux_eof = false;
        eof_reached = false;

        path_utf8.clear();
    }

    void clear_last_frame() noexcept {
        if (last_frame) {
            av_frame_unref(last_frame.get());
        }
        has_last_frame = false;
        last_pts = AV_NOPTS_VALUE;
        last_requested_target = AV_NOPTS_VALUE;
    }

    [[nodiscard]] bool seek_to(std::int64_t target) {
        clear_last_frame();
        demux_eof = false;
        eof_reached = false;

        const int rc = av_seek_frame(in.get(), v_index, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(debug) << "FrameSampler could not seek " << path_utf8
                                     << " to target " << target << ": " << err_str(rc);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool decode_forward_until(std::int64_t target) {
        clear_last_frame();

        for (;;) {
            // Drain any ready decoded frames before pulling more packets so
            // avcodec_send_packet never drops a packet with EAGAIN and buffered
            // frames from prior drain/reordering are consumed sequentially.
            int rc = avcodec_receive_frame(dec.get(), last_frame.get());
            if (rc == 0) {
                const std::int64_t pts = extract_frame_pts(last_frame.get());
                if (pts == AV_NOPTS_VALUE || pts >= target) {
                    has_last_frame = true;
                    last_pts = pts;
                    last_requested_target = target;
                    return true;
                }
                av_frame_unref(last_frame.get());
                continue;
            }
            if (rc == AVERROR_EOF) {
                eof_reached = true;
                clear_last_frame();
                return false;
            }
            if (rc != AVERROR(EAGAIN)) {
                BOOST_LOG_TRIVIAL(error) << "FrameSampler decode failed for " << path_utf8
                                         << ": " << err_str(rc);
                clear_last_frame();
                return false;
            }

            if (demux_eof) {
                eof_reached = true;
                clear_last_frame();
                return false;
            }

            // Decoder needs input; read packets until a video packet or EOF.
            for (;;) {
                rc = av_read_frame(in.get(), pkt.get());
                if (rc == AVERROR_EOF) {
                    demux_eof = true;
                    const int flush_rc = avcodec_send_packet(dec.get(), nullptr);
                    if (flush_rc < 0 && flush_rc != AVERROR_EOF) {
                        BOOST_LOG_TRIVIAL(error) << "FrameSampler drain failed for " << path_utf8
                                                 << ": " << err_str(flush_rc);
                        clear_last_frame();
                        return false;
                    }
                    break;
                }
                if (rc < 0) {
                    BOOST_LOG_TRIVIAL(error) << "FrameSampler read failed for " << path_utf8
                                             << ": " << err_str(rc);
                    clear_last_frame();
                    return false;
                }
                if (pkt->stream_index != v_index) {
                    av_packet_unref(pkt.get());
                    continue;
                }

                const int send_rc = avcodec_send_packet(dec.get(), pkt.get());
                av_packet_unref(pkt.get());
                if (send_rc < 0 && send_rc != AVERROR(EAGAIN)) {
                    BOOST_LOG_TRIVIAL(error) << "FrameSampler packet decode failed for "
                                             << path_utf8 << ": " << err_str(send_rc);
                    clear_last_frame();
                    return false;
                }
                break;
            }
        }
    }

    [[nodiscard]] bool should_decode_forward(double timestamp_sec, std::int64_t target) noexcept {
        if (eof_reached || !has_last_frame || last_pts == AV_NOPTS_VALUE || target <= last_pts) {
            return false;
        }

        const double last_pts_sec = static_cast<double>(last_pts) * av_q2d(time_base);
        AVStream* stream = in->streams[v_index];
        if (stream && avformat_index_get_entries_count(stream) > 0) {
            const int kf_idx = av_index_search_timestamp(stream, target, AVSEEK_FLAG_BACKWARD);
            if (kf_idx >= 0) {
                const AVIndexEntry* kf = avformat_index_get_entry(stream, kf_idx);
                if (kf && (kf->flags & AVINDEX_KEYFRAME) != 0) {
                    if (kf->timestamp > last_pts) {
                        const double kf_sec = static_cast<double>(kf->timestamp) * av_q2d(time_base);
                        // Seek past intermediate GOPs unless the keyframe is virtually adjacent.
                        return (kf_sec - last_pts_sec) <= kMinSeekSkipSec;
                    }
                    // Same GOP as current decoder position: keep decoding forward.
                    return true;
                }
            }
        }

        return (timestamp_sec - last_pts_sec) <= kUnindexedForwardAdvanceSec;
    }

    [[nodiscard]] bool decode_to_target(double timestamp_sec, std::int64_t target) {
        core::ScopedProfile prof{"av.sampler.decode"};

        if (!eof_reached && has_last_frame && last_pts != AV_NOPTS_VALUE) {
            if (target >= last_requested_target && last_pts >= target) {
                return true;
            }
            if (should_decode_forward(timestamp_sec, target)) {
                return decode_forward_until(target);
            }
        }

        if (!seek_to(target)) {
            return false;
        }
        return decode_forward_until(target);
    }

    [[nodiscard]] bool ensure_sws(int frame_w,
                                  int frame_h,
                                  AVPixelFormat src_fmt,
                                  AVPixelFormat dst_fmt) {
        if (sws && sws_width == frame_w && sws_height == frame_h &&
            sws_fmt == src_fmt && sws_dst_fmt == dst_fmt) {
            return true;
        }
        sws.reset(sws_getContext(frame_w, frame_h, src_fmt,
                                 frame_w, frame_h, dst_fmt,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws) {
            sws_width = 0;
            sws_height = 0;
            sws_fmt = AV_PIX_FMT_NONE;
            sws_dst_fmt = AV_PIX_FMT_NONE;
            BOOST_LOG_TRIVIAL(error) << "FrameSampler could not create SwsContext for "
                                     << path_utf8;
            return false;
        }
        sws_width = frame_w;
        sws_height = frame_h;
        sws_fmt = src_fmt;
        sws_dst_fmt = dst_fmt;
        return true;
    }

    [[nodiscard]] std::optional<FrameBuffer> convert_last_frame_to_bgr24(double requested_ts) {
        core::ScopedProfile prof{"av.sampler.sws_bgr24"};

        if (!has_last_frame || !last_frame) {
            return std::nullopt;
        }

        const int frame_w = last_frame->width;
        const int frame_h = last_frame->height;
        const auto src_fmt = static_cast<AVPixelFormat>(last_frame->format);
        if (frame_w <= 0 || frame_h <= 0 || src_fmt == AV_PIX_FMT_NONE) {
            return std::nullopt;
        }
        if (frame_w > std::numeric_limits<int>::max() / static_cast<int>(kBgraBytesPerPixel)) {
            return std::nullopt;
        }

        const int stride = frame_w * static_cast<int>(kBgr24BytesPerPixel);
        const std::size_t total_bytes =
            static_cast<std::size_t>(stride) * static_cast<std::size_t>(frame_h);

        FrameBuffer buf;
        buf.width = frame_w;
        buf.height = frame_h;
        buf.stride = stride;
        buf.timestamp_sec = (last_pts != AV_NOPTS_VALUE)
                                ? (static_cast<double>(last_pts) * av_q2d(time_base))
                                : requested_ts;
        buf.bgr24.resize(total_bytes);

        const bool ok = bgr_converter.convert(
            frame_w, frame_h, src_fmt == AV_PIX_FMT_YUV420P, buf.bgr24,
            [this, frame_w, frame_h, src_fmt](bool to_bgra,
                                              std::uint8_t* dst,
                                              int dst_stride) -> bool {
                const AVPixelFormat dst_fmt = to_bgra ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGR24;
                if (!ensure_sws(frame_w, frame_h, src_fmt, dst_fmt)) {
                    return false;
                }
                std::array<std::uint8_t*, kSwsPlaneCount> dst_data{
                    dst, nullptr, nullptr, nullptr};
                std::array<int, kSwsPlaneCount> dst_linesize{dst_stride, 0, 0, 0};
                return sws_scale(sws.get(),
                                 last_frame->data,
                                 last_frame->linesize,
                                 kFirstRowIndex,
                                 frame_h,
                                 dst_data.data(),
                                 dst_linesize.data()) > 0;
            });

        if (!ok) {
            BOOST_LOG_TRIVIAL(error) << "FrameSampler pixel conversion failed for " << path_utf8;
            return std::nullopt;
        }
        return buf;
    }
};

FrameSampler::FrameSampler() : impl_(std::make_unique<Impl>()) {}

FrameSampler::~FrameSampler() = default;

FrameSampler::FrameSampler(FrameSampler&&) noexcept = default;

FrameSampler& FrameSampler::operator=(FrameSampler&&) noexcept = default;

bool FrameSampler::open(const fs::path& video_path) noexcept {
    core::ScopedProfile prof{"av.sampler.open"};
    try {
        if (!impl_) {
            impl_ = std::make_unique<Impl>();
        }
        impl_->reset();

        auto cleanup = core::make_scope_guard([this]() noexcept { impl_->reset(); });

        const std::string path_utf8 = core::to_utf8(video_path);
        int rc = avformat_open_input(core::out_ptr(impl_->in), path_utf8.c_str(), nullptr, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "FrameSampler could not open " << path_utf8 << ": "
                                     << err_str(rc);
            return false;
        }

        rc = avformat_find_stream_info(impl_->in.get(), nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "FrameSampler found no stream info in " << path_utf8
                                     << ": " << err_str(rc);
            return false;
        }

        const int v_index = av_find_best_stream(
            impl_->in.get(), AVMEDIA_TYPE_VIDEO, kAutoStreamIndex, kAutoStreamIndex, nullptr, 0);
        if (v_index < 0 || static_cast<unsigned int>(v_index) >= impl_->in->nb_streams) {
            BOOST_LOG_TRIVIAL(error) << "FrameSampler found no video stream in " << path_utf8;
            return false;
        }

        const AVStream* stream = impl_->in->streams[v_index];
        if (!stream || !stream->codecpar ||
            (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0 ||
            stream->time_base.num <= 0 || stream->time_base.den <= 0) {
            BOOST_LOG_TRIVIAL(error) << "FrameSampler invalid video stream metadata in "
                                     << path_utf8;
            return false;
        }

        const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            BOOST_LOG_TRIVIAL(error) << "FrameSampler found no video decoder for " << path_utf8;
            return false;
        }

        impl_->dec.reset(avcodec_alloc_context3(decoder));
        if (!impl_->dec) {
            return false;
        }

        rc = avcodec_parameters_to_context(impl_->dec.get(), stream->codecpar);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "FrameSampler could not copy codec parameters for "
                                     << path_utf8 << ": " << err_str(rc);
            return false;
        }
        impl_->dec->pkt_timebase = stream->time_base;
        impl_->dec->thread_count = kAutoThreadCount;
        impl_->dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        rc = avcodec_open2(impl_->dec.get(), decoder, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "FrameSampler could not open decoder for " << path_utf8
                                     << ": " << err_str(rc);
            return false;
        }

        impl_->pkt = detail::make_packet();
        impl_->last_frame = detail::make_frame();
        if (!impl_->pkt || !impl_->last_frame) {
            return false;
        }

        impl_->v_index = v_index;
        impl_->time_base = stream->time_base;
        impl_->duration_sec = compute_duration_sec(impl_->in.get(), stream);
        impl_->width = (impl_->dec->width > 0) ? impl_->dec->width : stream->codecpar->width;
        impl_->height = (impl_->dec->height > 0) ? impl_->dec->height : stream->codecpar->height;
        impl_->path_utf8 = path_utf8;

        cleanup.dismiss();
        return true;
    } catch (const std::exception& e) {
        log_error_nothrow("FrameSampler open failed: ", e.what());
        return false;
    } catch (...) {
        log_error_nothrow("FrameSampler open failed: ", "unknown error");
        return false;
    }
}

void FrameSampler::close() noexcept {
    if (impl_) {
        impl_->reset();
    }
}

bool FrameSampler::is_open() const noexcept {
    return impl_ && impl_->is_open();
}

double FrameSampler::duration_sec() const noexcept {
    return (impl_ && impl_->is_open()) ? impl_->duration_sec : 0.0;
}

int FrameSampler::width() const noexcept {
    return (impl_ && impl_->is_open()) ? impl_->width : 0;
}

int FrameSampler::height() const noexcept {
    return (impl_ && impl_->is_open()) ? impl_->height : 0;
}

std::optional<FrameBuffer> FrameSampler::sample_at(double timestamp_sec) noexcept {
    try {
        if (!impl_ || !impl_->is_open()) {
            return std::nullopt;
        }
        if (!std::isfinite(timestamp_sec) || timestamp_sec < kMinTimestampSec) {
            return std::nullopt;
        }
        if (impl_->duration_sec > 0.0 && timestamp_sec > impl_->duration_sec + kEofMarginSec) {
            return std::nullopt;
        }
        if (timestamp_sec > kMaxRescaleSec) {
            return std::nullopt;
        }

        const auto target_us = static_cast<std::int64_t>(
            std::llround(timestamp_sec * static_cast<double>(AV_TIME_BASE)));
        const std::int64_t target = av_rescale_q(target_us, kAvTimeBaseRational, impl_->time_base);

        if (!impl_->decode_to_target(timestamp_sec, target)) {
            return std::nullopt;
        }
        return impl_->convert_last_frame_to_bgr24(timestamp_sec);
    } catch (const std::exception& e) {
        log_error_nothrow("FrameSampler sample_at failed: ", e.what());
        return std::nullopt;
    } catch (...) {
        log_error_nothrow("FrameSampler sample_at failed: ", "unknown error");
        return std::nullopt;
    }
}

std::size_t FrameSampler::sample_timestamps(
    std::span<const double> timestamps_sec,
    const std::function<void(std::size_t, FrameBuffer&&)>& callback) {
    if (!is_open() || timestamps_sec.empty()) {
        return 0;
    }

    std::size_t decoded = 0;
    for (std::size_t i = 0; i < timestamps_sec.size(); ++i) {
        auto frame = sample_at(timestamps_sec[i]);
        if (!frame) {
            continue;
        }
        ++decoded;
        if (callback) {
            callback(i, std::move(*frame));
        }
    }
    return decoded;
}

}  // namespace av
