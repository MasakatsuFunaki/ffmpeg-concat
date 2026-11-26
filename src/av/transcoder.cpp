#include "av/transcoder.h"

#include "av/av_error.h"
#include "av/av_handles.h"
#include "core/format_util.h"
#include "core/path_util.h"
#include "core/scoped_handle.h"
#include "ffmpeg/progress_reporter.h"
#include "platform/locale_guard.h"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace av {

namespace {

constexpr int kNoStreamIndex = -1;
constexpr int kAutoStreamIndex = -1;
constexpr int kAutoThreadCount = 0;
constexpr unsigned kPrimaryPadIndex = 0;
constexpr std::int64_t kUnknownPacketPos = -1;
constexpr std::size_t kChannelLayoutBufSize = 128;
constexpr AVRational kDefaultSar{1, 1};
constexpr std::chrono::milliseconds kProgressInterval{200};
constexpr std::string_view kVideoOutPad = "outv";
constexpr std::string_view kAudioOutPad = "outa";
constexpr std::string_view kAudioOutPadToken = "[outa]";
constexpr std::string_view kVideoPassthroughOnly = "[in0v]null[outv]";
constexpr std::string_view kVideoAudioPassthrough = "[in0v]null[outv];[in0a]anull[outa]";
constexpr char kAudioPassthroughGraph[] = "[in0a]anull[outa]";

enum class AudioMode {
    FilterGraph,
    StreamCopy,
    None,
};

[[nodiscard]] bool graph_has_audio_output(std::string_view filter_description) noexcept {
    return filter_description.find(kAudioOutPadToken) != std::string_view::npos;
}

[[nodiscard]] AudioMode resolve_audio_mode(const TranscodeRequest& req) noexcept {
    if (req.audio_copy) {
        return AudioMode::StreamCopy;
    }
    if (graph_has_audio_output(req.filter_description)) {
        return AudioMode::FilterGraph;
    }
    return AudioMode::None;
}

[[nodiscard]] std::string_view trim_ascii(std::string_view sv) noexcept {
    constexpr std::string_view kWhitespace = " \t\r\n";
    const std::size_t first = sv.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = sv.find_last_not_of(kWhitespace);
    return sv.substr(first, last - first + 1);
}

[[nodiscard]] bool is_video_passthrough_filter(std::string_view filter_description,
                                               AudioMode audio_mode) noexcept {
    const std::string_view trimmed = trim_ascii(filter_description);
    if (audio_mode == AudioMode::FilterGraph) {
        return trimmed == kVideoAudioPassthrough;
    }
    return trimmed == kVideoPassthroughOnly;
}

// Callback invoked by libavcodec once stream pixel formats are known. Picking
// CUDA keeps decoding on NVDEC; otherwise fall back to the first software fmt.
AVPixelFormat pick_cuda(AVCodecContext*, const AVPixelFormat* formats) {
    std::string offered;
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const char* name = av_get_pix_fmt_name(*p);
        if (!offered.empty()) {
            offered += ", ";
        }
        offered += (name ? name : "?");
        if (*p == AV_PIX_FMT_CUDA) {
            BOOST_LOG_TRIVIAL(debug) << "Decoding on the GPU (offered: " << offered << "...)";
            return *p;
        }
    }
    BOOST_LOG_TRIVIAL(warning) << "GPU decoding unavailable for this stream, using software "
                                  "(offered: "
                               << offered << ")";
    return formats[0];
}

// Downloads a GPU-resident frame to system memory; software frames pass through.
[[nodiscard]] detail::FramePtr to_software(detail::FramePtr frame) {
    if (!frame || frame->format != AV_PIX_FMT_CUDA) {
        return frame;
    }

    detail::FramePtr sw = detail::make_frame();
    if (!sw) {
        return nullptr;
    }
    const int rc = av_hwframe_transfer_data(sw.get(), frame.get(), 0);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not download frame from GPU: " << err_str(rc);
        return nullptr;
    }
    av_frame_copy_props(sw.get(), frame.get());
    return sw;
}

template <typename T>
    requires std::is_enum_v<T> || std::integral<T>
[[nodiscard]] std::vector<T> supported(const AVCodec* codec, AVCodecConfig config) {
    const void* list = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, config, 0, &list, &count) < 0 || !list) {
        return {};
    }
    const auto* typed = static_cast<const T*>(list);
    // Some codecs report a sentinel-terminated list with count == 0.
    if (count <= 0) {
        std::vector<T> out;
        for (int i = 0; typed[i] != static_cast<T>(-1); ++i) {
            out.push_back(typed[i]);
        }
        return out;
    }
    const std::span<const T> items{typed, static_cast<std::size_t>(count)};
    return {items.begin(), items.end()};
}

// Opened input file and its associated decoders and priming buffers (Rule of Zero).
// Reverse member destruction frees queued frames/packets -> vdec -> adec -> fmt.
struct Input {
    detail::FormatInputPtr fmt;
    int v_index = kNoStreamIndex;
    int a_index = kNoStreamIndex;
    detail::CodecContextPtr adec;
    detail::CodecContextPtr vdec;
    AVFilterContext* vsrc = nullptr;            // non-owning; owned by Pipeline::graph_
    AVFilterContext* asrc = nullptr;            // non-owning; owned by Pipeline::graph_
    std::vector<detail::FramePtr> queued_v;     // decoded before the graph existed
    std::vector<detail::FramePtr> queued_a;
    std::vector<detail::PacketPtr> queued_pkts;  // audio packets read while priming for copy
};

[[nodiscard]] bool supports_device(const AVCodec* codec, AVHWDeviceType type) noexcept {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            return false;
        }
        if (config->device_type == type &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            return true;
        }
    }
}

// Prefers a decoder implementing CUDA HW device contexts when requested, since
// avcodec_find_decoder() returns the first registered decoder (e.g. software
// libaom for AV1) rather than a GPU-capable one.
[[nodiscard]] const AVCodec* find_decoder(AVCodecID id, bool prefer_cuda) noexcept {
    if (prefer_cuda) {
        void* iter = nullptr;
        while (const AVCodec* codec = av_codec_iterate(&iter)) {
            if (codec->id != id || !av_codec_is_decoder(codec)) {
                continue;
            }
            if (supports_device(codec, AV_HWDEVICE_TYPE_CUDA)) {
                return codec;
            }
        }
    }
    return avcodec_find_decoder(id);
}

[[nodiscard]] detail::CodecContextPtr open_decoder(const AVFormatContext* fmt,
                                                   int index,
                                                   AVBufferRef* hw_device) {
    const AVStream* st = fmt->streams[index];
    const AVCodec* dec = find_decoder(st->codecpar->codec_id, hw_device != nullptr);
    if (!dec) {
        BOOST_LOG_TRIVIAL(error) << "No decoder for stream " << index;
        return nullptr;
    }

    detail::CodecContextPtr ctx{avcodec_alloc_context3(dec)};
    if (!ctx) {
        return nullptr;
    }

    int rc = avcodec_parameters_to_context(ctx.get(), st->codecpar);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not copy codec parameters: " << err_str(rc);
        return nullptr;
    }
    ctx->pkt_timebase = st->time_base;

    // Enable automatic multithreaded software decoding to match ffmpeg CLI behavior.
    ctx->thread_count = kAutoThreadCount;
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (hw_device && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        ctx->hw_device_ctx = av_buffer_ref(hw_device);
        ctx->get_format = pick_cuda;
    }

    rc = avcodec_open2(ctx.get(), dec, nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not open decoder: " << err_str(rc);
        return nullptr;
    }
    return ctx;
}

[[nodiscard]] bool codec_supports_pix_fmt(const AVCodec* codec, AVPixelFormat target) {
    const auto pix_fmts = supported<AVPixelFormat>(codec, AV_CODEC_CONFIG_PIX_FORMAT);
    return std::ranges::find(pix_fmts, target) != pix_fmts.end();
}

[[nodiscard]] bool is_nvenc_compatible_sw_format(const AVCodec* vcodec, AVPixelFormat sw_fmt) {
    switch (sw_fmt) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_YUV420P:
            return true;
        case AV_PIX_FMT_P010:
        case AV_PIX_FMT_P016:
            return codec_supports_pix_fmt(vcodec, AV_PIX_FMT_P010) ||
                   codec_supports_pix_fmt(vcodec, AV_PIX_FMT_P016);
        default:
            return false;
    }
}

[[nodiscard]] AVRational resolve_sar(const AVFrame* frame, const AVStream* stream) noexcept {
    if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0) {
        return frame->sample_aspect_ratio;
    }
    if (stream->sample_aspect_ratio.num > 0 && stream->sample_aspect_ratio.den > 0) {
        return stream->sample_aspect_ratio;
    }
    return kDefaultSar;
}

[[nodiscard]] AVRational resolve_frame_rate(const AVStream* stream) noexcept {
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        return stream->avg_frame_rate;
    }
    return stream->r_frame_rate;
}

void apply_color_metadata(AVCodecContext* enc,
                          const AVFrame* frame,
                          const AVCodecParameters* par) noexcept {
    const AVColorRange range =
        (frame->color_range != AVCOL_RANGE_UNSPECIFIED) ? frame->color_range : par->color_range;
    if (range != AVCOL_RANGE_UNSPECIFIED) {
        enc->color_range = range;
    }

    const AVColorPrimaries primaries = (frame->color_primaries != AVCOL_PRI_UNSPECIFIED)
                                           ? frame->color_primaries
                                           : par->color_primaries;
    if (primaries != AVCOL_PRI_UNSPECIFIED) {
        enc->color_primaries = primaries;
    }

    const AVColorTransferCharacteristic trc =
        (frame->color_trc != AVCOL_TRC_UNSPECIFIED) ? frame->color_trc : par->color_trc;
    if (trc != AVCOL_TRC_UNSPECIFIED) {
        enc->color_trc = trc;
    }

    const AVColorSpace space =
        (frame->colorspace != AVCOL_SPC_UNSPECIFIED) ? frame->colorspace : par->color_space;
    if (space != AVCOL_SPC_UNSPECIFIED) {
        enc->colorspace = space;
    }

    const AVChromaLocation chroma = (frame->chroma_location != AVCHROMA_LOC_UNSPECIFIED)
                                        ? frame->chroma_location
                                        : par->chroma_location;
    if (chroma != AVCHROMA_LOC_UNSPECIFIED) {
        enc->chroma_sample_location = chroma;
    }
}

// Pushes one packet through `dec`, appending every decoded frame to `out`.
// When `keep_hw_frames` is true, CUDA hardware frames remain in GPU memory.
[[nodiscard]] bool decode_into(AVCodecContext* dec,
                               const AVPacket* pkt,
                               std::vector<detail::FramePtr>& out,
                               bool keep_hw_frames) {
    int rc = avcodec_send_packet(dec, pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
        BOOST_LOG_TRIVIAL(error) << "Decode failed: " << err_str(rc);
        return false;
    }
    for (;;) {
        detail::FramePtr frame = detail::make_frame();
        if (!frame) {
            return false;
        }
        rc = avcodec_receive_frame(dec, frame.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            return true;
        }
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Decode failed: " << err_str(rc);
            return false;
        }
        if (!keep_hw_frames) {
            frame = to_software(std::move(frame));
            if (!frame) {
                return false;
            }
        }
        out.push_back(std::move(frame));
    }
}

class Pipeline {
public:
    Pipeline(const TranscodeRequest& req, bool& created_output) noexcept
        : req_(req),
          created_output_(created_output),
          audio_mode_(resolve_audio_mode(req)) {}

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    [[nodiscard]] bool run(ProgressReporter& reporter, double total_sec);

private:
    [[nodiscard]] bool can_attempt_zero_copy() const;
    [[nodiscard]] bool open_inputs();
    [[nodiscard]] bool prime_inputs();
    [[nodiscard]] bool prime_one(Input& input, bool keep_hw_frames);
    [[nodiscard]] bool build_graph();
    [[nodiscard]] bool open_encoders();
    [[nodiscard]] bool open_output();
    [[nodiscard]] bool push_frames(AVFilterContext* src, std::span<const detail::FramePtr> frames);
    [[nodiscard]] bool encode_video_frames(std::vector<detail::FramePtr>& frames);
    [[nodiscard]] bool feed_input(std::size_t index);
    [[nodiscard]] bool drain_graph();
    [[nodiscard]] bool encode_video(AVFrame* frame);
    [[nodiscard]] bool encode_audio(AVFrame* frame);
    [[nodiscard]] bool copy_audio_packet(const Input& in, AVPacket* pkt);
    void report(bool force);

    const TranscodeRequest& req_;
    bool& created_output_;
    const AudioMode audio_mode_;
    bool gpu_zero_copy_video_ = false;
    ProgressReporter* reporter_ = nullptr;
    double total_sec_ = 0.0;

    // Member order enforces safe reverse-dependency teardown (Rule of Zero):
    // scratch buffers -> encoders (incl. venc_->hw_frames_ctx) -> filter graph
    // -> output muxer -> inputs (queued CUDA frames -> vdec -> adec -> fmt) -> hw_device_.
    detail::BufferRefPtr hw_device_;
    std::vector<std::unique_ptr<Input>> inputs_;

    detail::FormatOutputPtr out_;
    AVStream* vstream_ = nullptr;
    AVStream* astream_ = nullptr;

    detail::FilterGraphPtr graph_;
    AVFilterContext* vsink_ = nullptr;
    AVFilterContext* asink_ = nullptr;

    const AVCodec* vcodec_ = nullptr;
    const AVCodec* acodec_ = nullptr;
    detail::CodecContextPtr aenc_;
    detail::CodecContextPtr venc_;

    detail::FramePtr drain_frame_;
    detail::PacketPtr encode_pkt_;

    std::int64_t video_frames_ = 0;
    double out_sec_ = 0.0;
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point last_report_{};
};

bool Pipeline::can_attempt_zero_copy() const {
    return req_.inputs.size() == 1 && hw_device_ != nullptr &&
           is_video_passthrough_filter(req_.filter_description, audio_mode_) &&
           codec_supports_pix_fmt(vcodec_, AV_PIX_FMT_CUDA);
}

bool Pipeline::open_inputs() {
    if (req_.hwaccel_cuda) {
        const int rc = av_hwdevice_ctx_create(
            core::out_ptr(hw_device_), AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(warning) << "CUDA device unavailable, decoding in software: "
                                       << err_str(rc);
            hw_device_.reset();
        }
    }

    inputs_.reserve(req_.inputs.size());
    for (const auto& path : req_.inputs) {
        auto in = std::make_unique<Input>();
        const std::string path_str = core::to_utf8(path);

        int rc = avformat_open_input(core::out_ptr(in->fmt), path_str.c_str(), nullptr, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open " << path_str << ": " << err_str(rc);
            return false;
        }
        rc = avformat_find_stream_info(in->fmt.get(), nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "No stream info in " << path_str << ": " << err_str(rc);
            return false;
        }

        in->v_index = av_find_best_stream(
            in->fmt.get(), AVMEDIA_TYPE_VIDEO, kAutoStreamIndex, kAutoStreamIndex, nullptr, 0);
        if (audio_mode_ != AudioMode::None) {
            in->a_index = av_find_best_stream(
                in->fmt.get(), AVMEDIA_TYPE_AUDIO, kAutoStreamIndex, kAutoStreamIndex, nullptr, 0);
        }
        if (in->v_index < 0) {
            BOOST_LOG_TRIVIAL(error) << path_str << " has no video stream.";
            return false;
        }
        if (audio_mode_ == AudioMode::FilterGraph && in->a_index < 0) {
            BOOST_LOG_TRIVIAL(error)
                << path_str << " has no audio stream, which the requested filter graph needs.";
            return false;
        }

        in->vdec = open_decoder(in->fmt.get(), in->v_index, hw_device_.get());
        if (!in->vdec) {
            return false;
        }
        if (audio_mode_ == AudioMode::FilterGraph && in->a_index >= 0) {
            in->adec = open_decoder(in->fmt.get(), in->a_index, nullptr);
            if (!in->adec) {
                return false;
            }
        }

        inputs_.push_back(std::move(in));
    }
    return true;
}

bool Pipeline::prime_inputs() {
    const bool candidate_zero_copy = can_attempt_zero_copy();

    for (auto& in : inputs_) {
        if (prime_one(*in, candidate_zero_copy)) {
            continue;
        }
        if (!hw_device_) {
            return false;
        }

        // Hardware-only decoders fail outright on unsupported profiles rather
        // than falling back automatically; reopen this input in software.
        BOOST_LOG_TRIVIAL(warning)
            << "GPU decoding did not work for this input; falling back to software for it.";
        in->queued_v.clear();
        in->queued_a.clear();
        in->queued_pkts.clear();
        in->vdec.reset();
        in->vdec = open_decoder(in->fmt.get(), in->v_index, nullptr);
        if (!in->vdec) {
            return false;
        }
        if (in->adec) {
            in->adec.reset();
            in->adec = open_decoder(in->fmt.get(), in->a_index, nullptr);
            if (!in->adec) {
                return false;
            }
        }
        if (avformat_seek_file(in->fmt.get(),
                               kAutoStreamIndex,
                               std::numeric_limits<std::int64_t>::min(),
                               0,
                               std::numeric_limits<std::int64_t>::max(),
                               0) < 0) {
            return false;
        }

        if (!prime_one(*in, false)) {
            return false;
        }
    }

    if (candidate_zero_copy) {
        Input& in0 = *inputs_[0];
        const AVFrame* vf = in0.queued_v.front().get();
        const bool all_cuda = std::ranges::all_of(
            in0.queued_v, [](const detail::FramePtr& frame) noexcept {
                return frame && frame->format == AV_PIX_FMT_CUDA;
            });
        AVBufferRef* hw_frames = vf->hw_frames_ctx ? vf->hw_frames_ctx : in0.vdec->hw_frames_ctx;
        bool can_zero_copy = false;
        if (all_cuda && hw_frames != nullptr && hw_frames->data != nullptr) {
            const auto* hwfc = reinterpret_cast<const AVHWFramesContext*>(hw_frames->data);
            can_zero_copy = is_nvenc_compatible_sw_format(vcodec_, hwfc->sw_format);
        }

        if (can_zero_copy) {
            gpu_zero_copy_video_ = true;
            BOOST_LOG_TRIVIAL(info)
                << "NVDEC -> NVENC zero-copy CUDA passthrough active (bypassing CPU video filter graph).";
        } else {
            gpu_zero_copy_video_ = false;
            for (auto& frame : in0.queued_v) {
                frame = to_software(std::move(frame));
                if (!frame) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool Pipeline::prime_one(Input& input, bool keep_hw_frames) {
    // Buffer sources require each input's true decoded frame format, which
    // hardware decoding reveals only after delivering a frame (e.g. NV12 vs
    // YUV420P). Decode one frame per active stream up front and queue it.
    detail::PacketPtr pkt = detail::make_packet();
    if (!pkt) {
        return false;
    }

    const bool need_audio = (audio_mode_ == AudioMode::FilterGraph);
    while (input.queued_v.empty() || (need_audio && input.queued_a.empty())) {
        const int rc = av_read_frame(input.fmt.get(), pkt.get());
        if (rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Read failed while priming: " << err_str(rc);
            return false;
        }

        bool ok = true;
        if (pkt->stream_index == input.v_index) {
            ok = decode_into(input.vdec.get(), pkt.get(), input.queued_v, keep_hw_frames);
        } else if (input.adec && pkt->stream_index == input.a_index) {
            ok = decode_into(input.adec.get(), pkt.get(), input.queued_a, false);
        } else if (audio_mode_ == AudioMode::StreamCopy && pkt->stream_index == input.a_index) {
            input.queued_pkts.push_back(std::move(pkt));
            pkt = detail::make_packet();
            if (!pkt) {
                return false;
            }
            continue;
        }
        av_packet_unref(pkt.get());
        if (!ok) {
            return false;
        }
    }

    if (input.queued_v.empty() || (need_audio && input.queued_a.empty())) {
        BOOST_LOG_TRIVIAL(error)
            << "Could not decode a first frame of every stream; the input may be truncated.";
        return false;
    }
    return true;
}

bool Pipeline::build_graph() {
    if (gpu_zero_copy_video_ && audio_mode_ != AudioMode::FilterGraph) {
        return true;
    }

    graph_.reset(avfilter_graph_alloc());
    if (!graph_) {
        return false;
    }

    for (const std::size_t i : std::views::iota(std::size_t{0}, inputs_.size())) {
        Input& in = *inputs_[i];

        if (!gpu_zero_copy_video_) {
            const AVFrame* vf = in.queued_v.front().get();
            const AVStream* vst = in.fmt->streams[in.v_index];
            const AVRational sar = resolve_sar(vf, vst);
            const AVRational rate = resolve_frame_rate(vst);

            const std::string vargs = std::format(
                "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}:frame_rate={}/{}",
                vf->width, vf->height, vf->format,
                vst->time_base.num, vst->time_base.den,
                sar.num, sar.den,
                rate.num, rate.den);
            const std::string vname = std::format("in{}v", i);

            const int rc = avfilter_graph_create_filter(&in.vsrc,
                                                        avfilter_get_by_name("buffer"),
                                                        vname.c_str(),
                                                        vargs.c_str(),
                                                        nullptr,
                                                        graph_.get());
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Could not create " << vname << ": " << err_str(rc);
                return false;
            }
        }

        if (audio_mode_ != AudioMode::FilterGraph) {
            continue;
        }

        const AVFrame* af = in.queued_a.front().get();
        const AVStream* ast = in.fmt->streams[in.a_index];

        std::array<char, kChannelLayoutBufSize> layout{};
        av_channel_layout_describe(&af->ch_layout, layout.data(), layout.size());
        const char* sample_fmt_name =
            av_get_sample_fmt_name(static_cast<AVSampleFormat>(af->format));
        const std::string aargs = std::format(
            "time_base={}/{}:sample_rate={}:sample_fmt={}:channel_layout={}",
            ast->time_base.num, ast->time_base.den,
            af->sample_rate,
            std::string_view{sample_fmt_name ? sample_fmt_name : "none"},
            std::string_view{layout.data()});
        const std::string aname = std::format("in{}a", i);

        const int rc = avfilter_graph_create_filter(&in.asrc,
                                                    avfilter_get_by_name("abuffer"),
                                                    aname.c_str(),
                                                    aargs.c_str(),
                                                    nullptr,
                                                    graph_.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not create " << aname << ": " << err_str(rc);
            return false;
        }
    }

    int rc = 0;
    if (!gpu_zero_copy_video_) {
        rc = avfilter_graph_create_filter(&vsink_,
                                          avfilter_get_by_name("buffersink"),
                                          "outv",
                                          nullptr,
                                          nullptr,
                                          graph_.get());
        if (rc < 0) {
            return false;
        }

        // Constrain the video sink to formats accepted by the selected encoder so
        // the graph inserts scale conversions automatically.
        auto pix_fmts = supported<AVPixelFormat>(vcodec_, AV_CODEC_CONFIG_PIX_FORMAT);
        if (pix_fmts.empty()) {
            pix_fmts.push_back(AV_PIX_FMT_YUV420P);
        }
        const auto pix_fmts_bytes = static_cast<int>(pix_fmts.size() * sizeof(AVPixelFormat));
        pix_fmts.push_back(AV_PIX_FMT_NONE);
        rc = av_opt_set_bin(vsink_,
                            "pix_fmts",
                            reinterpret_cast<const std::uint8_t*>(pix_fmts.data()),
                            pix_fmts_bytes,
                            AV_OPT_SEARCH_CHILDREN);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not constrain video sink: " << err_str(rc);
            return false;
        }
    }

    if (audio_mode_ == AudioMode::FilterGraph) {
        rc = avfilter_graph_create_filter(&asink_,
                                          avfilter_get_by_name("abuffersink"),
                                          "outa",
                                          nullptr,
                                          nullptr,
                                          graph_.get());
        if (rc < 0) {
            return false;
        }

        auto sample_fmts = supported<AVSampleFormat>(acodec_, AV_CODEC_CONFIG_SAMPLE_FORMAT);
        if (sample_fmts.empty()) {
            sample_fmts.push_back(AV_SAMPLE_FMT_FLTP);
        }
        const auto sample_fmts_bytes =
            static_cast<int>(sample_fmts.size() * sizeof(AVSampleFormat));
        sample_fmts.push_back(AV_SAMPLE_FMT_NONE);
        rc = av_opt_set_bin(asink_,
                            "sample_fmts",
                            reinterpret_cast<const std::uint8_t*>(sample_fmts.data()),
                            sample_fmts_bytes,
                            AV_OPT_SEARCH_CHILDREN);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not constrain audio sink: " << err_str(rc);
            return false;
        }
    }

    // Parse the filter chain and bind open pads by label so wiring is
    // independent of pad ordering.
    const char* graph_desc =
        gpu_zero_copy_video_ ? kAudioPassthroughGraph : req_.filter_description.c_str();
    detail::FilterInOutPtr parsed_inputs;
    detail::FilterInOutPtr parsed_outputs;
    rc = avfilter_graph_parse2(graph_.get(),
                               graph_desc,
                               core::out_ptr(parsed_inputs),
                               core::out_ptr(parsed_outputs));
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not parse filter graph \""
                                 << graph_desc << "\": " << err_str(rc);
        return false;
    }

    for (AVFilterInOut* cur = parsed_inputs.get(); cur; cur = cur->next) {
        const std::string_view pad_name = cur->name ? std::string_view{cur->name} : std::string_view{};
        AVFilterContext* src = nullptr;
        for (const std::size_t i : std::views::iota(std::size_t{0}, inputs_.size())) {
            if (pad_name == std::format("in{}v", i)) {
                src = inputs_[i]->vsrc;
                break;
            }
            if (pad_name == std::format("in{}a", i)) {
                src = inputs_[i]->asrc;
                break;
            }
        }
        if (!src) {
            BOOST_LOG_TRIVIAL(error) << "Filter graph asks for unknown input [" << pad_name << "]";
            return false;
        }
        rc = avfilter_link(src, kPrimaryPadIndex, cur->filter_ctx, static_cast<unsigned>(cur->pad_idx));
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not link [" << pad_name << "]: " << err_str(rc);
            return false;
        }
    }

    for (AVFilterInOut* cur = parsed_outputs.get(); cur; cur = cur->next) {
        const std::string_view pad_name = cur->name ? std::string_view{cur->name} : std::string_view{};
        AVFilterContext* sink = nullptr;
        if (pad_name == kVideoOutPad) {
            sink = vsink_;
        } else if (pad_name == kAudioOutPad) {
            sink = asink_;
        }
        if (!sink) {
            BOOST_LOG_TRIVIAL(error) << "Filter graph produces unknown output [" << pad_name << "]";
            return false;
        }
        rc = avfilter_link(cur->filter_ctx, static_cast<unsigned>(cur->pad_idx), sink, kPrimaryPadIndex);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not link [" << pad_name << "]: " << err_str(rc);
            return false;
        }
    }

    rc = avfilter_graph_config(graph_.get(), nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Filter graph is not configurable: " << err_str(rc);
        return false;
    }
    return true;
}

bool Pipeline::open_encoders() {
    venc_.reset(avcodec_alloc_context3(vcodec_));
    if (!venc_) {
        return false;
    }

    if (gpu_zero_copy_video_) {
        const Input& in0 = *inputs_[0];
        const AVFrame* vf = in0.queued_v.front().get();
        const AVStream* vst = in0.fmt->streams[in0.v_index];
        AVBufferRef* hw_frames = vf->hw_frames_ctx ? vf->hw_frames_ctx : in0.vdec->hw_frames_ctx;
        if (!hw_frames) {
            return false;
        }

        venc_->width = vf->width;
        venc_->height = vf->height;
        venc_->pix_fmt = AV_PIX_FMT_CUDA;
        venc_->hw_frames_ctx = av_buffer_ref(hw_frames);
        if (!venc_->hw_frames_ctx) {
            return false;
        }
        venc_->time_base = vst->time_base;
        venc_->framerate = resolve_frame_rate(vst);
        venc_->sample_aspect_ratio = resolve_sar(vf, vst);
        venc_->bit_rate = req_.video_bit_rate;
        venc_->thread_count = kAutoThreadCount;
        apply_color_metadata(venc_.get(), vf, vst->codecpar);
    } else {
        venc_->width = av_buffersink_get_w(vsink_);
        venc_->height = av_buffersink_get_h(vsink_);
        venc_->pix_fmt = static_cast<AVPixelFormat>(av_buffersink_get_format(vsink_));
        venc_->time_base = av_buffersink_get_time_base(vsink_);
        venc_->framerate = av_buffersink_get_frame_rate(vsink_);
        venc_->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(vsink_);
        venc_->bit_rate = req_.video_bit_rate;
        venc_->thread_count = kAutoThreadCount;
    }

    if (audio_mode_ != AudioMode::FilterGraph) {
        return true;
    }

    aenc_.reset(avcodec_alloc_context3(acodec_));
    if (!aenc_) {
        return false;
    }

    aenc_->sample_rate = av_buffersink_get_sample_rate(asink_);
    aenc_->sample_fmt = static_cast<AVSampleFormat>(av_buffersink_get_format(asink_));
    aenc_->bit_rate = req_.audio_bit_rate;
    aenc_->time_base = AVRational{1, aenc_->sample_rate};
    const int rc = av_buffersink_get_ch_layout(asink_, &aenc_->ch_layout);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not read the audio layout: " << err_str(rc);
        return false;
    }
    return true;
}

bool Pipeline::open_output() {
    const std::string out_path_str = core::to_utf8(req_.output);
    int rc = avformat_alloc_output_context2(
        core::out_ptr(out_), nullptr, nullptr, out_path_str.c_str());
    if (rc < 0 || !out_) {
        BOOST_LOG_TRIVIAL(error) << "Could not create the output container: " << err_str(rc);
        return false;
    }

    // Containers like MP4 store codec extradata in the global header rather than in-band.
    if (out_->oformat->flags & AVFMT_GLOBALHEADER) {
        venc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (aenc_) {
            aenc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
    }

    detail::DictionaryPtr vopts;
    for (const auto& [key, val] : req_.video_options) {
        av_dict_set(core::inout_ptr(vopts), key.c_str(), val.c_str(), 0);
    }
    rc = avcodec_open2(venc_.get(), vcodec_, core::inout_ptr(vopts));
    if (av_dict_count(vopts.get()) > 0) {
        const AVDictionaryEntry* e =
            av_dict_get(vopts.get(), "", nullptr, AV_DICT_IGNORE_SUFFIX);
        BOOST_LOG_TRIVIAL(warning) << "Encoder " << req_.video_encoder
                                   << " ignored option '" << (e ? e->key : "?") << "'.";
    }
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not open encoder " << req_.video_encoder
                                 << ": " << err_str(rc);
        return false;
    }

    if (aenc_) {
        rc = avcodec_open2(aenc_.get(), acodec_, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open encoder " << req_.audio_encoder
                                     << ": " << err_str(rc);
            return false;
        }

        // Let the audio sink emit exact encoder frame sizes (e.g. 1024 for AAC).
        if (aenc_->frame_size > 0) {
            av_buffersink_set_frame_size(asink_, aenc_->frame_size);
        }
    }

    vstream_ = avformat_new_stream(out_.get(), nullptr);
    if (!vstream_) {
        return false;
    }
    rc = avcodec_parameters_from_context(vstream_->codecpar, venc_.get());
    if (rc < 0) {
        return false;
    }
    vstream_->time_base = venc_->time_base;

    const bool copy_audio = (audio_mode_ == AudioMode::StreamCopy) && inputs_[0]->a_index >= 0;
    if (aenc_ || copy_audio) {
        astream_ = avformat_new_stream(out_.get(), nullptr);
        if (!astream_) {
            return false;
        }
        if (aenc_) {
            rc = avcodec_parameters_from_context(astream_->codecpar, aenc_.get());
            astream_->time_base = aenc_->time_base;
        } else {
            const AVStream* src = inputs_[0]->fmt->streams[inputs_[0]->a_index];
            rc = avcodec_parameters_copy(astream_->codecpar, src->codecpar);
            astream_->codecpar->codec_tag = 0;  // let the muxer choose its own tag
            astream_->time_base = src->time_base;
        }
        if (rc < 0) {
            return false;
        }
    }

    if (!(out_->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&out_->pb, out_path_str.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not create " << out_path_str << ": " << err_str(rc);
            return false;
        }
        created_output_ = true;
    }

    rc = avformat_write_header(out_.get(), nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not write the container header: " << err_str(rc);
        return false;
    }
    return true;
}

bool Pipeline::encode_video(AVFrame* frame) {
    if (frame) {
        const AVRational src_tb = vsink_
            ? av_buffersink_get_time_base(vsink_)
            : inputs_[0]->fmt->streams[inputs_[0]->v_index]->time_base;
        std::int64_t pts = frame->pts;
        if (!vsink_ && pts == AV_NOPTS_VALUE) {
            pts = frame->best_effort_timestamp;
        }
        if (pts != AV_NOPTS_VALUE) {
            frame->pts = av_rescale_q(pts, src_tb, venc_->time_base);
        }
        frame->pict_type = AV_PICTURE_TYPE_NONE;
    }
    int rc = avcodec_send_frame(venc_.get(), frame);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Video encode failed: " << err_str(rc);
        return false;
    }

    bool wrote_packet = false;
    for (;;) {
        rc = avcodec_receive_packet(venc_.get(), encode_pkt_.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            if (!vsink_ && wrote_packet) {
                report(false);
            }
            return true;
        }
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Video encode failed: " << err_str(rc);
            return false;
        }
        encode_pkt_->stream_index = vstream_->index;
        av_packet_rescale_ts(encode_pkt_.get(), venc_->time_base, vstream_->time_base);
        if (encode_pkt_->pts != AV_NOPTS_VALUE) {
            out_sec_ = encode_pkt_->pts * av_q2d(vstream_->time_base);
        }
        rc = av_interleaved_write_frame(out_.get(), encode_pkt_.get());
        av_packet_unref(encode_pkt_.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write a video packet: " << err_str(rc);
            return false;
        }
        ++video_frames_;
        wrote_packet = true;
    }
}

bool Pipeline::encode_audio(AVFrame* frame) {
    if (frame) {
        frame->pts = av_rescale_q(frame->pts, av_buffersink_get_time_base(asink_), aenc_->time_base);
    }
    int rc = avcodec_send_frame(aenc_.get(), frame);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Audio encode failed: " << err_str(rc);
        return false;
    }

    for (;;) {
        rc = avcodec_receive_packet(aenc_.get(), encode_pkt_.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            return true;
        }
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Audio encode failed: " << err_str(rc);
            return false;
        }
        encode_pkt_->stream_index = astream_->index;
        av_packet_rescale_ts(encode_pkt_.get(), aenc_->time_base, astream_->time_base);
        rc = av_interleaved_write_frame(out_.get(), encode_pkt_.get());
        av_packet_unref(encode_pkt_.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write an audio packet: " << err_str(rc);
            return false;
        }
    }
}

bool Pipeline::copy_audio_packet(const Input& in, AVPacket* pkt) {
    if (!astream_) {
        return true;  // source has no audio track
    }

    const AVStream* src = in.fmt->streams[in.a_index];
    pkt->stream_index = astream_->index;
    av_packet_rescale_ts(pkt, src->time_base, astream_->time_base);
    pkt->pos = kUnknownPacketPos;

    const int rc = av_interleaved_write_frame(out_.get(), pkt);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not write a copied audio packet: " << err_str(rc);
        return false;
    }
    return true;
}

bool Pipeline::drain_graph() {
    if (!vsink_ && !asink_) {
        return true;
    }
    while (vsink_) {
        const int rc = av_buffersink_get_frame(vsink_, drain_frame_.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Filter graph failed: " << err_str(rc);
            return false;
        }
        const bool ok = encode_video(drain_frame_.get());
        av_frame_unref(drain_frame_.get());
        if (!ok) {
            return false;
        }
    }
    while (asink_) {
        const int rc = av_buffersink_get_frame(asink_, drain_frame_.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Filter graph failed: " << err_str(rc);
            return false;
        }
        const bool ok = encode_audio(drain_frame_.get());
        av_frame_unref(drain_frame_.get());
        if (!ok) {
            return false;
        }
    }

    report(false);
    return true;
}

bool Pipeline::push_frames(AVFilterContext* src, std::span<const detail::FramePtr> frames) {
    if (frames.empty()) {
        return true;
    }
    return std::ranges::all_of(frames, [this, src](const detail::FramePtr& frame) {
        return av_buffersrc_add_frame_flags(src, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF) >= 0 &&
               drain_graph();
    });
}

bool Pipeline::encode_video_frames(std::vector<detail::FramePtr>& frames) {
    for (auto& frame : frames) {
        const bool ok = encode_video(frame.get());
        frame.reset();
        if (!ok) {
            frames.clear();
            return false;
        }
    }
    frames.clear();
    return true;
}

bool Pipeline::feed_input(std::size_t index) {
    Input& in = *inputs_[index];

    if (gpu_zero_copy_video_) {
        if (!encode_video_frames(in.queued_v)) {
            return false;
        }
    } else if (!push_frames(in.vsrc, in.queued_v)) {
        return false;
    }
    if (in.asrc && !push_frames(in.asrc, in.queued_a)) {
        return false;
    }
    for (auto& queued_pkt : in.queued_pkts) {
        if (!copy_audio_packet(in, queued_pkt.get())) {
            return false;
        }
    }
    in.queued_v.clear();
    in.queued_a.clear();
    in.queued_pkts.clear();

    detail::PacketPtr pkt = detail::make_packet();
    if (!pkt) {
        return false;
    }
    std::vector<detail::FramePtr> frames;

    for (;;) {
        const int rc = av_read_frame(in.fmt.get(), pkt.get());
        if (rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Read failed: " << err_str(rc);
            return false;
        }

        AVFilterContext* target = nullptr;
        bool direct_video = false;
        bool ok = true;
        if (pkt->stream_index == in.v_index) {
            ok = decode_into(in.vdec.get(), pkt.get(), frames, gpu_zero_copy_video_);
            if (gpu_zero_copy_video_) {
                direct_video = true;
            } else {
                target = in.vsrc;
            }
        } else if (in.adec && pkt->stream_index == in.a_index) {
            ok = decode_into(in.adec.get(), pkt.get(), frames, false);
            target = in.asrc;
        } else if (audio_mode_ == AudioMode::StreamCopy && pkt->stream_index == in.a_index) {
            ok = copy_audio_packet(in, pkt.get());
        }
        av_packet_unref(pkt.get());
        if (!ok) {
            return false;
        }

        if (direct_video) {
            ok = encode_video_frames(frames);
        } else if (target) {
            ok = push_frames(target, frames);
        }
        frames.clear();
        if (!ok) {
            return false;
        }
    }

    // Flush decoders and close segment pads so concat advances to the next input.
    if (!decode_into(in.vdec.get(), nullptr, frames, gpu_zero_copy_video_)) {
        return false;
    }
    if (gpu_zero_copy_video_) {
        if (!encode_video_frames(frames)) {
            return false;
        }
    } else if (!push_frames(in.vsrc, frames)) {
        return false;
    }
    frames.clear();

    if (in.adec) {
        if (!decode_into(in.adec.get(), nullptr, frames, false) || !push_frames(in.asrc, frames)) {
            return false;
        }
        frames.clear();
    }

    if (in.vsrc && av_buffersrc_add_frame(in.vsrc, nullptr) < 0) {
        return false;
    }
    if (in.asrc && av_buffersrc_add_frame(in.asrc, nullptr) < 0) {
        return false;
    }
    return drain_graph();
}

void Pipeline::report(bool force) {
    if (!reporter_ || total_sec_ <= 0.0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force && (now - last_report_) < kProgressInterval) {
        return;
    }
    last_report_ = now;

    const double elapsed = std::chrono::duration<double>(now - started_).count();
    if (elapsed <= 0.0) {
        return;
    }

    const ProgressParser::Snapshot snap{
        .current_sec = out_sec_,
        .out_time = core::format_hms(out_sec_),
        .fps = std::to_string(static_cast<int>(video_frames_ / elapsed)),
        .speed = core::format_speed(out_sec_ / elapsed),
    };
    reporter_->update(snap, total_sec_);
}

bool Pipeline::run(ProgressReporter& reporter, double total_sec) {
    reporter_ = &reporter;
    total_sec_ = total_sec;

    vcodec_ = avcodec_find_encoder_by_name(req_.video_encoder.c_str());
    if (!vcodec_) {
        BOOST_LOG_TRIVIAL(error) << "No encoder named " << req_.video_encoder;
        return false;
    }
    if (audio_mode_ == AudioMode::FilterGraph) {
        acodec_ = avcodec_find_encoder_by_name(req_.audio_encoder.c_str());
        if (!acodec_) {
            BOOST_LOG_TRIVIAL(error) << "No encoder named " << req_.audio_encoder;
            return false;
        }
    }

    if (!open_inputs() || !prime_inputs() || !build_graph() || !open_encoders() || !open_output()) {
        return false;
    }

    drain_frame_ = detail::make_frame();
    encode_pkt_ = detail::make_packet();
    if (!drain_frame_ || !encode_pkt_) {
        return false;
    }

    started_ = std::chrono::steady_clock::now();
    last_report_ = started_;

    for (const std::size_t i : std::views::iota(std::size_t{0}, inputs_.size())) {
        if (!feed_input(i)) {
            return false;
        }
    }

    if (!drain_graph() || !encode_video(nullptr)) {
        return false;
    }
    if (aenc_ && !encode_audio(nullptr)) {
        return false;
    }

    const int rc = av_write_trailer(out_.get());
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not finalize " << core::to_utf8(req_.output) << ": "
                                 << err_str(rc);
        return false;
    }
    report(true);
    return true;
}

}  // namespace

bool transcode(const TranscodeRequest& req, ProgressReporter& reporter, double total_sec) {
    if (req.inputs.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Nothing to encode: no inputs.";
        return false;
    }
    try {
        const platform::ScopedCNumericLocale numeric_locale;
        bool created_output = false;
        auto remove_partial_output = core::make_scope_guard([&req, &created_output]() noexcept {
            if (created_output) {
                std::error_code ec;
                fs::remove(req.output, ec);
            }
        });

        Pipeline pipeline(req, created_output);
        const bool ok = pipeline.run(reporter, total_sec);
        if (ok) {
            remove_partial_output.dismiss();
            reporter.finish();
        } else {
            reporter.abort();
        }
        return ok;
    } catch (const std::exception& e) {
        reporter.abort();
        BOOST_LOG_TRIVIAL(error) << "Transcode failed: " << e.what();
        return false;
    } catch (...) {
        reporter.abort();
        BOOST_LOG_TRIVIAL(error) << "Transcode failed with an unknown error.";
        return false;
    }
}

}  // namespace av
