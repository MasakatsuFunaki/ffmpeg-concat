#include "av/transcoder.h"

#include "av/av_error.h"
#include "ffmpeg/progress_reporter.h"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <memory>
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace av {
namespace {

// --------------------------------------------------------------------------
// Locale
// --------------------------------------------------------------------------

// libavfilter parses every number in a graph description through the C locale,
// and libraries in this process switch it: indicators calls setlocale, which on
// a European system makes "." stop being the decimal separator and breaks filter
// arguments. As a subprocess ffmpeg was insulated from that; linked in, it is
// not. Pin LC_NUMERIC for as long as libav* is parsing.
class ScopedCNumeric {
public:
    ScopedCNumeric() {
        const char* current = std::setlocale(LC_NUMERIC, nullptr);
        if (current) saved_ = current;
        std::setlocale(LC_NUMERIC, "C");
    }
    ~ScopedCNumeric() {
        if (!saved_.empty()) std::setlocale(LC_NUMERIC, saved_.c_str());
    }
    ScopedCNumeric(const ScopedCNumeric&) = delete;
    ScopedCNumeric& operator=(const ScopedCNumeric&) = delete;

private:
    std::string saved_;
};

// --------------------------------------------------------------------------
// RAII
// --------------------------------------------------------------------------

struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using FramePtr  = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

FramePtr  new_frame()  { return FramePtr(av_frame_alloc()); }
PacketPtr new_packet() { return PacketPtr(av_packet_alloc()); }

// --------------------------------------------------------------------------
// Hardware decoding
// --------------------------------------------------------------------------

// Chosen by libavcodec once it knows the stream's real formats. Taking CUDA
// keeps decoding on NVDEC; returning the first entry otherwise is the software
// fallback, which is what happens for any codec NVDEC does not implement.
AVPixelFormat pick_cuda(AVCodecContext*, const AVPixelFormat* formats) {
    std::string offered;
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        const char* name = av_get_pix_fmt_name(*p);
        offered += std::string(offered.empty() ? "" : ", ") + (name ? name : "?");
        if (*p == AV_PIX_FMT_CUDA) {
            BOOST_LOG_TRIVIAL(debug) << "Decoding on the GPU (offered: " << offered << "...)";
            return *p;
        }
    }
    BOOST_LOG_TRIVIAL(warning) << "GPU decoding unavailable for this stream, using software "
                                  "(offered: " << offered << ")";
    return formats[0];
}

// Bring a GPU-resident frame back to system memory; software frames pass through.
FramePtr to_software(FramePtr frame) {
    if (!frame || frame->format != AV_PIX_FMT_CUDA) return frame;

    FramePtr sw = new_frame();
    if (!sw) return nullptr;
    int rc = av_hwframe_transfer_data(sw.get(), frame.get(), 0);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not download frame from GPU: " << err_str(rc);
        return nullptr;
    }
    av_frame_copy_props(sw.get(), frame.get());
    return sw;
}

// --------------------------------------------------------------------------
// Encoder capability queries
// --------------------------------------------------------------------------

template <typename T>
std::vector<T> supported(const AVCodec* codec, AVCodecConfig config) {
    const void* list = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, config, 0, &list, &count) < 0 || !list) {
        return {};
    }
    const T* typed = static_cast<const T*>(list);
    // A null-terminated list is reported with count == 0 by some codecs.
    if (count <= 0) {
        std::vector<T> out;
        for (int i = 0; typed[i] != static_cast<T>(-1); ++i) out.push_back(typed[i]);
        return out;
    }
    return std::vector<T>(typed, typed + count);
}

// --------------------------------------------------------------------------
// One opened input file
// --------------------------------------------------------------------------

struct Input {
    AVFormatContext* fmt = nullptr;
    int v_index = -1;
    int a_index = -1;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    AVFilterContext* vsrc = nullptr;   // owned by the graph
    AVFilterContext* asrc = nullptr;   // owned by the graph
    std::vector<FramePtr> queued_v;    // decoded before the graph existed
    std::vector<FramePtr> queued_a;
    std::vector<PacketPtr> queued_pkts;  // audio read while priming, when copying

    ~Input() {
        if (vdec) avcodec_free_context(&vdec);
        if (adec) avcodec_free_context(&adec);
        if (fmt) avformat_close_input(&fmt);
    }
};

// Does this decoder implement the given hardware device type?
bool supports_device(const AVCodec* codec, AVHWDeviceType type) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) return false;
        if (config->device_type == type &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            return true;
        }
    }
}

// avcodec_find_decoder() returns whichever decoder for this codec id was
// registered first, and that is not necessarily one that can use the GPU: with
// no libdav1d in the build, AV1 resolves to libaom, a software-only decoder
// several times slower than NVDEC. When a hardware device is in play, look for a
// decoder that actually implements it before falling back to the default.
const AVCodec* find_decoder(AVCodecID id, bool prefer_cuda) {
    if (prefer_cuda) {
        void* iter = nullptr;
        while (const AVCodec* codec = av_codec_iterate(&iter)) {
            if (codec->id != id || !av_codec_is_decoder(codec)) continue;
            if (supports_device(codec, AV_HWDEVICE_TYPE_CUDA)) return codec;
        }
    }
    return avcodec_find_decoder(id);
}

bool open_decoder(AVFormatContext* fmt, int index, AVBufferRef* hw_device,
                  AVCodecContext** out) {
    const AVStream* st = fmt->streams[index];
    const AVCodec* dec = find_decoder(st->codecpar->codec_id, hw_device != nullptr);
    if (!dec) {
        BOOST_LOG_TRIVIAL(error) << "No decoder for stream " << index;
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) return false;

    int rc = avcodec_parameters_to_context(ctx, st->codecpar);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not copy codec parameters: " << err_str(rc);
        avcodec_free_context(&ctx);
        return false;
    }
    // Makes decoded frames carry timestamps in the stream's time base.
    ctx->pkt_timebase = st->time_base;

    // libavcodec defaults to a single thread; the ffmpeg CLI asks for automatic
    // threading and so must we, or software decoding runs on one core.
    ctx->thread_count = 0;
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (hw_device && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        ctx->hw_device_ctx = av_buffer_ref(hw_device);
        ctx->get_format = pick_cuda;
    }

    rc = avcodec_open2(ctx, dec, nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not open decoder: " << err_str(rc);
        avcodec_free_context(&ctx);
        return false;
    }
    *out = ctx;
    return true;
}

// Push one packet through a decoder, appending every frame it yields.
bool decode_into(AVCodecContext* dec, const AVPacket* pkt, std::vector<FramePtr>& out) {
    int rc = avcodec_send_packet(dec, pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) {
        BOOST_LOG_TRIVIAL(error) << "Decode failed: " << err_str(rc);
        return false;
    }
    for (;;) {
        FramePtr frame = new_frame();
        if (!frame) return false;
        rc = avcodec_receive_frame(dec, frame.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Decode failed: " << err_str(rc);
            return false;
        }
        FramePtr sw = to_software(std::move(frame));
        if (!sw) return false;
        out.push_back(std::move(sw));
    }
}

std::string format_hms(double seconds) {
    if (seconds < 0) seconds = 0;
    int total = static_cast<int>(seconds);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << (total / 3600) << ":"
        << std::setfill('0') << std::setw(2) << ((total % 3600) / 60) << ":"
        << std::setfill('0') << std::setw(2) << (total % 60);
    return oss.str();
}

// --------------------------------------------------------------------------
// The pipeline
// --------------------------------------------------------------------------

class Pipeline {
public:
    explicit Pipeline(const TranscodeRequest& req) : req_(req) {}

    ~Pipeline() {
        if (venc_) avcodec_free_context(&venc_);
        if (aenc_) avcodec_free_context(&aenc_);
        if (graph_) avfilter_graph_free(&graph_);
        if (out_) {
            if (out_->pb && !(out_->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_->pb);
            avformat_free_context(out_);
        }
        if (hw_device_) av_buffer_unref(&hw_device_);
    }

    bool run(ProgressReporter& reporter, double total_sec);

private:
    bool open_inputs();
    bool prime_inputs();          // decode enough to learn each input's real formats
    bool prime_one(Input& input);
    bool build_graph();
    bool open_encoders();
    bool open_output();
    bool feed_input(size_t index);
    bool drain_graph();
    bool encode_video(AVFrame* frame);
    bool encode_audio(AVFrame* frame);
    bool copy_audio_packet(const Input& in, AVPacket* pkt);
    void report(bool force);

    const TranscodeRequest& req_;
    const bool audio_through_graph_ = !req_.audio_copy;
    ProgressReporter* reporter_ = nullptr;
    double total_sec_ = 0.0;
    std::vector<std::unique_ptr<Input>> inputs_;
    AVBufferRef* hw_device_ = nullptr;

    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* vsink_ = nullptr;
    AVFilterContext* asink_ = nullptr;

    AVCodecContext* venc_ = nullptr;
    AVCodecContext* aenc_ = nullptr;
    const AVCodec* vcodec_ = nullptr;
    const AVCodec* acodec_ = nullptr;

    AVFormatContext* out_ = nullptr;
    AVStream* vstream_ = nullptr;
    AVStream* astream_ = nullptr;

    int64_t video_frames_ = 0;
    double out_sec_ = 0.0;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_report_;
};

bool Pipeline::open_inputs() {
    if (req_.hwaccel_cuda) {
        int rc = av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(warning) << "CUDA device unavailable, decoding in software: "
                                       << err_str(rc);
            hw_device_ = nullptr;
        }
    }

    for (const auto& path : req_.inputs) {
        auto in = std::make_unique<Input>();

        int rc = avformat_open_input(&in->fmt, path.string().c_str(), nullptr, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open " << path << ": " << err_str(rc);
            return false;
        }
        rc = avformat_find_stream_info(in->fmt, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "No stream info in " << path << ": " << err_str(rc);
            return false;
        }

        in->v_index = av_find_best_stream(in->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        in->a_index = av_find_best_stream(in->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (in->v_index < 0) {
            BOOST_LOG_TRIVIAL(error) << path << " has no video stream.";
            return false;
        }
        if (in->a_index < 0 && audio_through_graph_) {
            BOOST_LOG_TRIVIAL(error) << path
                << " has no audio stream, which the requested filter graph needs.";
            return false;
        }

        if (!open_decoder(in->fmt, in->v_index, hw_device_, &in->vdec)) return false;
        // Copied audio is never decoded, so it needs no decoder.
        if (in->a_index >= 0 && audio_through_graph_ &&
            !open_decoder(in->fmt, in->a_index, nullptr, &in->adec)) {
            return false;
        }

        inputs_.push_back(std::move(in));
    }
    return true;
}

bool Pipeline::prime_inputs() {
    for (auto& in : inputs_) {
        if (prime_one(*in)) continue;
        if (!hw_device_) return false;

        // The decoder chosen for the GPU may be hardware-only (the native AV1
        // decoder is), so a stream NVDEC cannot handle fails outright rather than
        // falling back on its own. Reopen this input in software and try once more.
        BOOST_LOG_TRIVIAL(warning) << "GPU decoding did not work for this input; "
                                      "falling back to software for it.";
        avcodec_free_context(&in->vdec);
        if (!open_decoder(in->fmt, in->v_index, nullptr, &in->vdec)) return false;
        if (in->adec) {
            avcodec_free_context(&in->adec);
            if (!open_decoder(in->fmt, in->a_index, nullptr, &in->adec)) return false;
        }
        in->queued_v.clear();
        in->queued_a.clear();
        in->queued_pkts.clear();
        if (avformat_seek_file(in->fmt, -1, INT64_MIN, 0, INT64_MAX, 0) < 0) return false;

        if (!prime_one(*in)) return false;
    }
    return true;
}

bool Pipeline::prime_one(Input& input) {
    // The filter graph's buffer sources must be created with each input's real
    // frame format, and hardware decoding only reveals it once a frame comes
    // back (NVDEC hands back NV12 where the software path gives YUV420P). So
    // decode one frame of each kind up front and keep it for later.
    {
        Input* in = &input;
        PacketPtr pkt = new_packet();
        if (!pkt) return false;

        while (in->queued_v.empty() || (audio_through_graph_ && in->queued_a.empty())) {
            int rc = av_read_frame(in->fmt, pkt.get());
            if (rc == AVERROR_EOF) break;
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Read failed while priming: " << err_str(rc);
                return false;
            }
            bool ok = true;
            if (pkt->stream_index == in->v_index) {
                ok = decode_into(in->vdec, pkt.get(), in->queued_v);
            } else if (in->adec && pkt->stream_index == in->a_index) {
                ok = decode_into(in->adec, pkt.get(), in->queued_a);
            } else if (pkt->stream_index == in->a_index) {
                in->queued_pkts.push_back(std::move(pkt));  // copied straight through later
                pkt = new_packet();
                if (!pkt) return false;
                continue;
            }
            av_packet_unref(pkt.get());
            if (!ok) return false;
        }

        if (in->queued_v.empty() || (audio_through_graph_ && in->queued_a.empty())) {
            BOOST_LOG_TRIVIAL(error) << "Could not decode a first frame of every stream; "
                                        "the input may be truncated.";
            return false;
        }
    }
    return true;
}

bool Pipeline::build_graph() {
    graph_ = avfilter_graph_alloc();
    if (!graph_) return false;

    // Buffer sources, described by the frames primed above.
    for (size_t i = 0; i < inputs_.size(); ++i) {
        Input& in = *inputs_[i];
        const AVFrame* vf = in.queued_v.front().get();
        const AVStream* vst = in.fmt->streams[in.v_index];

        AVRational sar = vf->sample_aspect_ratio;
        if (sar.num <= 0 || sar.den <= 0) sar = AVRational{1, 1};
        AVRational rate = vst->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) rate = vst->r_frame_rate;

        char args[512];
        std::snprintf(args, sizeof(args),
                      "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d",
                      vf->width, vf->height, vf->format,
                      vst->time_base.num, vst->time_base.den,
                      sar.num, sar.den, rate.num, rate.den);

        std::string vname = "in" + std::to_string(i) + "v";
        int rc = avfilter_graph_create_filter(&in.vsrc, avfilter_get_by_name("buffer"),
                                              vname.c_str(), args, nullptr, graph_);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not create " << vname << ": " << err_str(rc);
            return false;
        }

        if (!audio_through_graph_) continue;

        const AVFrame* af = in.queued_a.front().get();
        const AVStream* ast = in.fmt->streams[in.a_index];

        char layout[128];
        av_channel_layout_describe(&af->ch_layout, layout, sizeof(layout));
        char aargs[512];
        std::snprintf(aargs, sizeof(aargs),
                      "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                      ast->time_base.num, ast->time_base.den, af->sample_rate,
                      av_get_sample_fmt_name(static_cast<AVSampleFormat>(af->format)), layout);

        std::string aname = "in" + std::to_string(i) + "a";
        rc = avfilter_graph_create_filter(&in.asrc, avfilter_get_by_name("abuffer"),
                                          aname.c_str(), aargs, nullptr, graph_);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not create " << aname << ": " << err_str(rc);
            return false;
        }
    }

    int rc = avfilter_graph_create_filter(&vsink_, avfilter_get_by_name("buffersink"),
                                          "outv", nullptr, nullptr, graph_);
    if (rc < 0) return false;
    if (audio_through_graph_) {
        rc = avfilter_graph_create_filter(&asink_, avfilter_get_by_name("abuffersink"),
                                          "outa", nullptr, nullptr, graph_);
        if (rc < 0) return false;
    }

    // Constrain the sinks to what the chosen encoders accept, so the graph
    // inserts the needed scale/aresample conversions itself.
    auto pix_fmts = supported<AVPixelFormat>(vcodec_, AV_CODEC_CONFIG_PIX_FORMAT);
    if (pix_fmts.empty()) pix_fmts.push_back(AV_PIX_FMT_YUV420P);
    pix_fmts.push_back(AV_PIX_FMT_NONE);
    rc = av_opt_set_bin(vsink_, "pix_fmts", reinterpret_cast<const uint8_t*>(pix_fmts.data()),
                        static_cast<int>((pix_fmts.size() - 1) * sizeof(AVPixelFormat)),
                        AV_OPT_SEARCH_CHILDREN);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not constrain video sink: " << err_str(rc);
        return false;
    }

    if (asink_) {
        auto sample_fmts = supported<AVSampleFormat>(acodec_, AV_CODEC_CONFIG_SAMPLE_FORMAT);
        if (sample_fmts.empty()) sample_fmts.push_back(AV_SAMPLE_FMT_FLTP);
        sample_fmts.push_back(AV_SAMPLE_FMT_NONE);
        rc = av_opt_set_bin(asink_, "sample_fmts",
                            reinterpret_cast<const uint8_t*>(sample_fmts.data()),
                            static_cast<int>((sample_fmts.size() - 1) * sizeof(AVSampleFormat)),
                            AV_OPT_SEARCH_CHILDREN);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not constrain audio sink: " << err_str(rc);
            return false;
        }
    }

    // Parse the requested chain, then wire our sources and sinks onto the pads it
    // left open. Matching by label keeps this independent of pad ordering.
    AVFilterInOut* parsed_inputs = nullptr;
    AVFilterInOut* parsed_outputs = nullptr;
    rc = avfilter_graph_parse2(graph_, req_.filter_description.c_str(),
                               &parsed_inputs, &parsed_outputs);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not parse filter graph \""
                                 << req_.filter_description << "\": " << err_str(rc);
        return false;
    }

    bool ok = true;
    for (AVFilterInOut* cur = parsed_inputs; cur && ok; cur = cur->next) {
        AVFilterContext* src = nullptr;
        for (size_t i = 0; i < inputs_.size() && !src; ++i) {
            if (cur->name == std::string("in" + std::to_string(i) + "v")) src = inputs_[i]->vsrc;
            if (cur->name == std::string("in" + std::to_string(i) + "a")) src = inputs_[i]->asrc;
        }
        if (!src) {
            BOOST_LOG_TRIVIAL(error) << "Filter graph asks for unknown input [" << cur->name << "]";
            ok = false;
            break;
        }
        rc = avfilter_link(src, 0, cur->filter_ctx, cur->pad_idx);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not link [" << cur->name << "]: " << err_str(rc);
            ok = false;
        }
    }
    for (AVFilterInOut* cur = parsed_outputs; cur && ok; cur = cur->next) {
        AVFilterContext* sink = nullptr;
        if (cur->name == std::string("outv")) sink = vsink_;
        else if (cur->name == std::string("outa")) sink = asink_;
        if (!sink) {
            BOOST_LOG_TRIVIAL(error) << "Filter graph produces unknown output [" << cur->name << "]";
            ok = false;
            break;
        }
        rc = avfilter_link(cur->filter_ctx, cur->pad_idx, sink, 0);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not link [" << cur->name << "]: " << err_str(rc);
            ok = false;
        }
    }
    avfilter_inout_free(&parsed_inputs);
    avfilter_inout_free(&parsed_outputs);
    if (!ok) return false;

    rc = avfilter_graph_config(graph_, nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Filter graph is not configurable: " << err_str(rc);
        return false;
    }
    return true;
}

bool Pipeline::open_encoders() {
    venc_ = avcodec_alloc_context3(vcodec_);
    if (!venc_) return false;

    venc_->width               = av_buffersink_get_w(vsink_);
    venc_->height              = av_buffersink_get_h(vsink_);
    venc_->pix_fmt             = static_cast<AVPixelFormat>(av_buffersink_get_format(vsink_));
    venc_->time_base           = av_buffersink_get_time_base(vsink_);
    venc_->framerate           = av_buffersink_get_frame_rate(vsink_);
    venc_->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(vsink_);
    venc_->bit_rate            = req_.video_bit_rate;
    venc_->thread_count        = 0;   // automatic, as above

    if (!audio_through_graph_) return true;   // copied audio needs no encoder

    aenc_ = avcodec_alloc_context3(acodec_);
    if (!aenc_) return false;

    aenc_->sample_rate = av_buffersink_get_sample_rate(asink_);
    aenc_->sample_fmt  = static_cast<AVSampleFormat>(av_buffersink_get_format(asink_));
    aenc_->bit_rate    = req_.audio_bit_rate;
    aenc_->time_base   = AVRational{1, aenc_->sample_rate};
    int rc = av_buffersink_get_ch_layout(asink_, &aenc_->ch_layout);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not read the audio layout: " << err_str(rc);
        return false;
    }

    return true;
}

bool Pipeline::open_output() {
    int rc = avformat_alloc_output_context2(&out_, nullptr, nullptr, req_.output.string().c_str());
    if (rc < 0 || !out_) {
        BOOST_LOG_TRIVIAL(error) << "Could not create the output container: " << err_str(rc);
        return false;
    }

    // mp4 keeps codec extradata in the container header, so the encoders must be
    // told to emit it globally rather than in-band.
    if (out_->oformat->flags & AVFMT_GLOBALHEADER) {
        venc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (aenc_) aenc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* vopts = nullptr;
    for (const auto& kv : req_.video_options) {
        av_dict_set(&vopts, kv.first.c_str(), kv.second.c_str(), 0);
    }
    rc = avcodec_open2(venc_, vcodec_, &vopts);
    int leftover = av_dict_count(vopts);
    if (leftover > 0) {
        const AVDictionaryEntry* e = av_dict_get(vopts, "", nullptr, AV_DICT_IGNORE_SUFFIX);
        BOOST_LOG_TRIVIAL(warning) << "Encoder " << req_.video_encoder
                                   << " ignored option '" << (e ? e->key : "?") << "'.";
    }
    av_dict_free(&vopts);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not open encoder " << req_.video_encoder
                                 << ": " << err_str(rc);
        return false;
    }

    if (aenc_) {
        rc = avcodec_open2(aenc_, acodec_, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open encoder " << req_.audio_encoder
                                     << ": " << err_str(rc);
            return false;
        }

        // AAC consumes a fixed number of samples per call; let the sink hand out
        // exactly that many so no separate sample FIFO is needed.
        if (aenc_->frame_size > 0) {
            av_buffersink_set_frame_size(asink_, aenc_->frame_size);
        }
    }

    vstream_ = avformat_new_stream(out_, nullptr);
    if (!vstream_) return false;
    rc = avcodec_parameters_from_context(vstream_->codecpar, venc_);
    if (rc < 0) return false;
    vstream_->time_base = venc_->time_base;

    const bool copy_audio = !audio_through_graph_ && inputs_[0]->a_index >= 0;
    if (aenc_ || copy_audio) {
        astream_ = avformat_new_stream(out_, nullptr);
        if (!astream_) return false;
        if (aenc_) {
            rc = avcodec_parameters_from_context(astream_->codecpar, aenc_);
            astream_->time_base = aenc_->time_base;
        } else {
            const AVStream* src = inputs_[0]->fmt->streams[inputs_[0]->a_index];
            rc = avcodec_parameters_copy(astream_->codecpar, src->codecpar);
            astream_->codecpar->codec_tag = 0;   // let the muxer choose its own tag
            astream_->time_base = src->time_base;
        }
        if (rc < 0) return false;
    }

    if (!(out_->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&out_->pb, req_.output.string().c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not create " << req_.output << ": " << err_str(rc);
            return false;
        }
    }

    rc = avformat_write_header(out_, nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not write the container header: " << err_str(rc);
        return false;
    }
    return true;
}

bool Pipeline::encode_video(AVFrame* frame) {
    if (frame) {
        frame->pts = av_rescale_q(frame->pts, av_buffersink_get_time_base(vsink_), venc_->time_base);
        frame->pict_type = AV_PICTURE_TYPE_NONE;
    }
    int rc = avcodec_send_frame(venc_, frame);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Video encode failed: " << err_str(rc);
        return false;
    }
    PacketPtr pkt = new_packet();
    if (!pkt) return false;
    for (;;) {
        rc = avcodec_receive_packet(venc_, pkt.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Video encode failed: " << err_str(rc);
            return false;
        }
        pkt->stream_index = vstream_->index;
        av_packet_rescale_ts(pkt.get(), venc_->time_base, vstream_->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) {
            out_sec_ = pkt->pts * av_q2d(vstream_->time_base);
        }
        rc = av_interleaved_write_frame(out_, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write a video packet: " << err_str(rc);
            return false;
        }
        ++video_frames_;
    }
}

bool Pipeline::encode_audio(AVFrame* frame) {
    if (frame) {
        frame->pts = av_rescale_q(frame->pts, av_buffersink_get_time_base(asink_), aenc_->time_base);
    }
    int rc = avcodec_send_frame(aenc_, frame);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Audio encode failed: " << err_str(rc);
        return false;
    }
    PacketPtr pkt = new_packet();
    if (!pkt) return false;
    for (;;) {
        rc = avcodec_receive_packet(aenc_, pkt.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Audio encode failed: " << err_str(rc);
            return false;
        }
        pkt->stream_index = astream_->index;
        av_packet_rescale_ts(pkt.get(), aenc_->time_base, astream_->time_base);
        rc = av_interleaved_write_frame(out_, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write an audio packet: " << err_str(rc);
            return false;
        }
    }
}

bool Pipeline::copy_audio_packet(const Input& in, AVPacket* pkt) {
    if (!astream_) return true;   // the output has no audio track

    const AVStream* src = in.fmt->streams[in.a_index];
    pkt->stream_index = astream_->index;
    av_packet_rescale_ts(pkt, src->time_base, astream_->time_base);
    pkt->pos = -1;

    int rc = av_interleaved_write_frame(out_, pkt);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not write a copied audio packet: " << err_str(rc);
        return false;
    }
    return true;
}

bool Pipeline::drain_graph() {
    FramePtr frame = new_frame();
    if (!frame) return false;

    for (;;) {
        int rc = av_buffersink_get_frame(vsink_, frame.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Filter graph failed: " << err_str(rc);
            return false;
        }
        bool ok = encode_video(frame.get());
        av_frame_unref(frame.get());
        if (!ok) return false;
    }
    while (asink_) {
        int rc = av_buffersink_get_frame(asink_, frame.get());
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Filter graph failed: " << err_str(rc);
            return false;
        }
        bool ok = encode_audio(frame.get());
        av_frame_unref(frame.get());
        if (!ok) return false;
    }

    // Drain runs constantly while feeding, so this is where progress advances;
    // report() throttles itself so a long file still updates smoothly.
    report(false);
    return true;
}

bool Pipeline::feed_input(size_t index) {
    Input& in = *inputs_[index];

    // Frames decoded during priming come first, then the rest of the file.
    for (auto& frame : in.queued_v) {
        if (av_buffersrc_add_frame_flags(in.vsrc, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF) < 0) return false;
        if (!drain_graph()) return false;
    }
    for (auto& frame : in.queued_a) {
        if (av_buffersrc_add_frame_flags(in.asrc, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF) < 0) return false;
        if (!drain_graph()) return false;
    }
    for (auto& pkt : in.queued_pkts) {
        if (!copy_audio_packet(in, pkt.get())) return false;
    }
    in.queued_v.clear();
    in.queued_a.clear();
    in.queued_pkts.clear();

    PacketPtr pkt = new_packet();
    if (!pkt) return false;
    std::vector<FramePtr> frames;

    for (;;) {
        int rc = av_read_frame(in.fmt, pkt.get());
        if (rc == AVERROR_EOF) break;
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Read failed: " << err_str(rc);
            return false;
        }

        AVFilterContext* target = nullptr;
        bool ok = true;
        if (pkt->stream_index == in.v_index) {
            ok = decode_into(in.vdec, pkt.get(), frames);
            target = in.vsrc;
        } else if (in.adec && pkt->stream_index == in.a_index) {
            ok = decode_into(in.adec, pkt.get(), frames);
            target = in.asrc;
        } else if (pkt->stream_index == in.a_index) {
            ok = copy_audio_packet(in, pkt.get());
        }
        av_packet_unref(pkt.get());
        if (!ok) return false;

        for (auto& frame : frames) {
            if (av_buffersrc_add_frame_flags(target, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                return false;
            }
            if (!drain_graph()) return false;
        }
        frames.clear();
    }

    // Flush both decoders, then close this segment's pads so concat advances.
    if (!decode_into(in.vdec, nullptr, frames)) return false;
    for (auto& frame : frames) {
        if (av_buffersrc_add_frame_flags(in.vsrc, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF) < 0) return false;
        if (!drain_graph()) return false;
    }
    frames.clear();

    if (in.adec) {
        if (!decode_into(in.adec, nullptr, frames)) return false;
        for (auto& frame : frames) {
            if (av_buffersrc_add_frame_flags(in.asrc, frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF) < 0) return false;
            if (!drain_graph()) return false;
        }
        frames.clear();
    }

    // Closing the pads is what makes the concat filter advance to the next segment.
    if (av_buffersrc_add_frame(in.vsrc, nullptr) < 0) return false;
    if (in.asrc && av_buffersrc_add_frame(in.asrc, nullptr) < 0) return false;
    return drain_graph();
}

void Pipeline::report(bool force) {
    if (!reporter_ || total_sec_ <= 0) return;

    auto now = std::chrono::steady_clock::now();
    if (!force && now - last_report_ < std::chrono::milliseconds(200)) return;
    last_report_ = now;

    double elapsed = std::chrono::duration<double>(now - started_).count();
    if (elapsed <= 0) return;

    ProgressParser::Snapshot snap;
    snap.current_sec = out_sec_;
    snap.out_time = format_hms(out_sec_);
    snap.fps   = std::to_string(static_cast<int>(video_frames_ / elapsed));
    char speed[32];
    std::snprintf(speed, sizeof(speed), "%.3gx", out_sec_ / elapsed);
    snap.speed = speed;
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
    if (audio_through_graph_) {
        acodec_ = avcodec_find_encoder_by_name(req_.audio_encoder.c_str());
        if (!acodec_) {
            BOOST_LOG_TRIVIAL(error) << "No encoder named " << req_.audio_encoder;
            return false;
        }
    }

    if (!open_inputs()) return false;
    if (!prime_inputs()) return false;
    if (!build_graph()) return false;
    if (!open_encoders()) return false;
    if (!open_output()) return false;

    started_ = std::chrono::steady_clock::now();
    last_report_ = started_;

    for (size_t i = 0; i < inputs_.size(); ++i) {
        if (!feed_input(i)) return false;
    }

    if (!drain_graph()) return false;
    if (!encode_video(nullptr)) return false;             // flush
    if (aenc_ && !encode_audio(nullptr)) return false;

    int rc = av_write_trailer(out_);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not finalize " << req_.output << ": " << err_str(rc);
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
        ScopedCNumeric numeric_locale;
        Pipeline pipeline(req);
        bool ok = pipeline.run(reporter, total_sec);
        if (ok) {
            reporter.finish();
        } else {
            reporter.abort();
        }
        return ok;
    } catch (const std::exception& e) {
        reporter.abort();
        BOOST_LOG_TRIVIAL(error) << "Transcode failed: " << e.what();
        return false;
    }
}

}  // namespace av
