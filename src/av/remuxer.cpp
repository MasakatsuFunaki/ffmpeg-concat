#include "av/remuxer.h"

#include "av/av_error.h"
#include "av/av_handles.h"
#include "core/format_util.h"
#include "core/path_util.h"
#include "core/scoped_handle.h"
#include "ffmpeg/progress_reporter.h"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace av {

namespace {

constexpr int kUnmappedStream = -1;
constexpr int kAutoStreamIndex = -1;
constexpr int kSeekAllStreams = -1;
constexpr std::int64_t kUnknownPacketPos = -1;
constexpr std::int64_t kMinDtsStep = 1;
constexpr double kUnsetTimeSec = -1.0;
constexpr std::uint8_t kStreamActive = 0;
constexpr std::uint8_t kStreamFinished = 1;
constexpr std::chrono::milliseconds kProgressInterval{200};

// Best video/audio stream selection and parameters used for concat validation
// and per-clip packet remapping.
struct StreamSignature {
    int video_index = kUnmappedStream;
    int audio_index = kUnmappedStream;
    AVCodecID video_codec = AV_CODEC_ID_NONE;
    int width = 0;
    int height = 0;
    AVCodecID audio_codec = AV_CODEC_ID_NONE;

    [[nodiscard]] bool has_video() const noexcept { return video_index >= 0; }
    [[nodiscard]] bool has_audio() const noexcept { return audio_index >= 0; }
};

[[nodiscard]] const char* codec_name(AVCodecID id) noexcept {
    const char* name = avcodec_get_name(id);
    return name ? name : "unknown";
}

[[nodiscard]] StreamSignature inspect_streams(AVFormatContext* fmt) noexcept {
    StreamSignature sig;

    const int vid = av_find_best_stream(
        fmt, AVMEDIA_TYPE_VIDEO, kAutoStreamIndex, kAutoStreamIndex, nullptr, 0);
    if (vid >= 0) {
        sig.video_index = vid;
        const AVCodecParameters* par = fmt->streams[vid]->codecpar;
        sig.video_codec = par->codec_id;
        sig.width = par->width;
        sig.height = par->height;
    }

    const int aud = av_find_best_stream(
        fmt, AVMEDIA_TYPE_AUDIO, kAutoStreamIndex, kAutoStreamIndex, nullptr, 0);
    if (aud >= 0) {
        sig.audio_index = aud;
        sig.audio_codec = fmt->streams[aud]->codecpar->codec_id;
    }

    return sig;
}

[[nodiscard]] bool validate_clip_compatibility(std::size_t clip_index,
                                               const fs::path& clip_path,
                                               const StreamSignature& ref,
                                               const StreamSignature& cur) {
    std::vector<std::string> reasons;

    if (cur.has_video() != ref.has_video()) {
        reasons.push_back(std::format(
            "video stream presence mismatch (clip #0 {}, clip #{} {})",
            ref.has_video()
                ? std::format("has video ({} {}x{})",
                              codec_name(ref.video_codec), ref.width, ref.height)
                : std::string{"has no video"},
            clip_index,
            cur.has_video()
                ? std::format("has video ({} {}x{})",
                              codec_name(cur.video_codec), cur.width, cur.height)
                : std::string{"has no video"}));
    } else if (ref.has_video()) {
        if (cur.video_codec != ref.video_codec) {
            reasons.push_back(std::format(
                "video codec mismatch (clip #0 {}, clip #{} {})",
                codec_name(ref.video_codec), clip_index, codec_name(cur.video_codec)));
        }
        if (cur.width != ref.width || cur.height != ref.height) {
            reasons.push_back(std::format(
                "video dimensions mismatch (clip #0 {}x{}, clip #{} {}x{})",
                ref.width, ref.height, clip_index, cur.width, cur.height));
        }
    }

    if (cur.has_audio() != ref.has_audio()) {
        reasons.push_back(std::format(
            "audio stream presence mismatch (clip #0 {}, clip #{} {})",
            ref.has_audio() ? std::format("has audio ({})", codec_name(ref.audio_codec))
                            : std::string{"has no audio"},
            clip_index,
            cur.has_audio() ? std::format("has audio ({})", codec_name(cur.audio_codec))
                            : std::string{"has no audio"}));
    } else if (ref.has_audio() && cur.audio_codec != ref.audio_codec) {
        reasons.push_back(std::format(
            "audio codec mismatch (clip #0 {}, clip #{} {})",
            codec_name(ref.audio_codec), clip_index, codec_name(cur.audio_codec)));
    }

    if (reasons.empty()) {
        return true;
    }

    BOOST_LOG_TRIVIAL(error) << "Cannot stream-copy concatenate clip #" << clip_index
                             << " (" << core::to_utf8(clip_path) << "): "
                             << core::join(reasons, "; ")
                             << ". Re-encode with the transcode tool to join incompatible clips.";
    return false;
}

// Output container paired with its source-to-output stream index map.
class Output {
public:
    // Mirrors every audio and video stream from `source` in source order.
    [[nodiscard]] bool open(const fs::path& path,
                            const AVFormatContext* source,
                            bool& created_output) {
        std::vector<int> indices;
        indices.reserve(source->nb_streams);
        for (const unsigned i : std::views::iota(0u, source->nb_streams)) {
            const AVMediaType type = source->streams[i]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO) {
                indices.push_back(static_cast<int>(i));
            }
        }
        return open_selected(path, source, indices, created_output);
    }

    // Mirrors only the validated best video and best audio streams from `source`.
    [[nodiscard]] bool open(const fs::path& path,
                            const AVFormatContext* source,
                            const StreamSignature& sig,
                            bool& created_output) {
        std::vector<int> indices;
        if (sig.has_video()) {
            indices.push_back(sig.video_index);
        }
        if (sig.has_audio()) {
            indices.push_back(sig.audio_index);
        }
        if (!open_selected(path, source, indices, created_output)) {
            return false;
        }
        video_out_index_ = sig.has_video() ? mapped(sig.video_index) : kUnmappedStream;
        audio_out_index_ = sig.has_audio() ? mapped(sig.audio_index) : kUnmappedStream;
        return true;
    }

    [[nodiscard]] bool finish() {
        const int rc = av_write_trailer(fmt_.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not finalize the output: " << err_str(rc);
            return false;
        }
        return true;
    }

    [[nodiscard]] int mapped(int source_index) const noexcept {
        if (source_index < 0 || std::cmp_greater_equal(source_index, stream_map_.size())) {
            return kUnmappedStream;
        }
        return stream_map_[static_cast<std::size_t>(source_index)];
    }

    // Maps a packet from any validated clip's stream indices to output stream indices.
    [[nodiscard]] int map_for(const StreamSignature& sig, int source_index) const noexcept {
        if (sig.has_video() && source_index == sig.video_index) {
            return video_out_index_;
        }
        if (sig.has_audio() && source_index == sig.audio_index) {
            return audio_out_index_;
        }
        return kUnmappedStream;
    }

    [[nodiscard]] AVStream* stream(int out_index) const noexcept {
        return fmt_->streams[out_index];
    }

    [[nodiscard]] AVFormatContext* ctx() const noexcept { return fmt_.get(); }

private:
    [[nodiscard]] bool open_selected(const fs::path& path,
                                     const AVFormatContext* source,
                                     std::span<const int> source_indices,
                                     bool& created_output) {
        video_out_index_ = kUnmappedStream;
        audio_out_index_ = kUnmappedStream;

        const std::string path_str = core::to_utf8(path);
        int rc = avformat_alloc_output_context2(
            core::out_ptr(fmt_), nullptr, nullptr, path_str.c_str());
        if (rc < 0 || !fmt_) {
            BOOST_LOG_TRIVIAL(error) << "Could not create " << path_str << ": " << err_str(rc);
            return false;
        }

        stream_map_.assign(source->nb_streams, kUnmappedStream);
        for (const int src_index : source_indices) {
            if (src_index < 0 || std::cmp_greater_equal(src_index, source->nb_streams)) {
                continue;
            }
            const AVStream* in = source->streams[src_index];
            AVStream* out = avformat_new_stream(fmt_.get(), nullptr);
            if (!out) {
                return false;
            }
            rc = avcodec_parameters_copy(out->codecpar, in->codecpar);
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Could not copy stream parameters: " << err_str(rc);
                return false;
            }
            out->codecpar->codec_tag = 0;  // let the muxer pick a tag it supports
            out->time_base = in->time_base;
            stream_map_[static_cast<std::size_t>(src_index)] = out->index;
        }

        if (!(fmt_->oformat->flags & AVFMT_NOFILE)) {
            rc = avio_open(&fmt_->pb, path_str.c_str(), AVIO_FLAG_WRITE);
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Could not create " << path_str << ": " << err_str(rc);
                return false;
            }
            created_output = true;
        }

        rc = avformat_write_header(fmt_.get(), nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write the container header: " << err_str(rc);
            return false;
        }
        return true;
    }

    detail::FormatOutputPtr fmt_;
    std::vector<int> stream_map_;
    int video_out_index_ = kUnmappedStream;
    int audio_out_index_ = kUnmappedStream;
};

class InputFile {
public:
    [[nodiscard]] bool open(const fs::path& path) {
        const std::string path_str = core::to_utf8(path);
        int rc = avformat_open_input(core::out_ptr(fmt_), path_str.c_str(), nullptr, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open " << path_str << ": " << err_str(rc);
            return false;
        }
        rc = avformat_find_stream_info(fmt_.get(), nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "No stream info in " << path_str << ": " << err_str(rc);
            return false;
        }
        return true;
    }

    [[nodiscard]] AVFormatContext* ctx() const noexcept { return fmt_.get(); }

private:
    detail::FormatInputPtr fmt_;
};

[[nodiscard]] bool validate_concat_inputs(std::span<const fs::path> inputs,
                                          const StreamSignature& ref) {
    if (!ref.has_video() && !ref.has_audio()) {
        BOOST_LOG_TRIVIAL(error) << "Cannot stream-copy concatenate: clip #0 ("
                                 << core::to_utf8(inputs.front())
                                 << ") has neither a video nor an audio stream.";
        return false;
    }

    for (std::size_t i = 1; i < inputs.size(); ++i) {
        InputFile probe;
        if (!probe.open(inputs[i])) {
            return false;
        }
        const StreamSignature cur = inspect_streams(probe.ctx());
        if (!validate_clip_compatibility(i, inputs[i], ref, cur)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool run_concat_copy(std::span<const fs::path> inputs,
                                   const fs::path& out_path,
                                   ProgressReporter& reporter,
                                   double total_sec) {
    InputFile first;
    if (!first.open(inputs.front())) {
        return false;
    }

    const StreamSignature ref = inspect_streams(first.ctx());
    if (!validate_concat_inputs(inputs, ref)) {
        return false;
    }

    // Declared before `out` so `out` closes the Windows file handle before removal on failure.
    bool created_output = false;
    auto remove_partial_output = core::make_scope_guard([&out_path, &created_output]() noexcept {
        if (created_output) {
            std::error_code ec;
            fs::remove(out_path, ec);
        }
    });

    Output out;
    if (!out.open(out_path, first.ctx(), ref, created_output)) {
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    auto last_report = started;
    double written_sec = 0.0;  // end timestamp of everything written so far
    const std::size_t num_out_streams = out.ctx()->nb_streams;
    std::vector<std::int64_t> offsets(num_out_streams, 0);
    std::vector<std::int64_t> last_dts(num_out_streams, AV_NOPTS_VALUE);
    std::vector<std::int64_t> next_min_dts(num_out_streams, 0);

    detail::PacketPtr pkt = detail::make_packet();
    if (!pkt) {
        return false;
    }

    for (const std::size_t i : std::views::iota(std::size_t{0}, inputs.size())) {
        InputFile current;
        InputFile* source = &current;
        StreamSignature sig = ref;
        if (i == 0) {
            source = &first;
        } else {
            if (!current.open(inputs[i])) {
                return false;
            }
            sig = inspect_streams(current.ctx());
            if (!validate_clip_compatibility(i, inputs[i], ref, sig)) {
                return false;
            }
        }

        std::vector<std::int64_t> first_ts(source->ctx()->nb_streams, AV_NOPTS_VALUE);
        double segment_end = written_sec;

        for (;;) {
            int rc = av_read_frame(source->ctx(), pkt.get());
            if (rc == AVERROR_EOF) {
                break;
            }
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Read failed: " << err_str(rc);
                return false;
            }

            const int out_index = out.map_for(sig, pkt->stream_index);
            if (out_index < 0) {
                av_packet_unref(pkt.get());
                continue;
            }

            const auto in_idx = static_cast<std::size_t>(pkt->stream_index);
            const auto out_idx = static_cast<std::size_t>(out_index);
            const AVStream* src = source->ctx()->streams[in_idx];
            AVStream* dst = out.stream(out_index);

            const std::int64_t raw_ts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
            if (first_ts[in_idx] == AV_NOPTS_VALUE && raw_ts != AV_NOPTS_VALUE) {
                first_ts[in_idx] = raw_ts;
            }
            if (const std::int64_t base = first_ts[in_idx]; base != AV_NOPTS_VALUE) {
                if (pkt->pts != AV_NOPTS_VALUE) {
                    pkt->pts -= base;
                }
                if (pkt->dts != AV_NOPTS_VALUE) {
                    pkt->dts -= base;
                }
            }

            pkt->stream_index = out_index;
            av_packet_rescale_ts(pkt.get(), src->time_base, dst->time_base);
            if (pkt->pts != AV_NOPTS_VALUE) {
                pkt->pts += offsets[out_idx];
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                pkt->dts += offsets[out_idx];
            }
            if (pkt->dts != AV_NOPTS_VALUE && last_dts[out_idx] != AV_NOPTS_VALUE &&
                pkt->dts <= last_dts[out_idx]) {
                const std::int64_t bump = (last_dts[out_idx] + kMinDtsStep) - pkt->dts;
                pkt->dts += bump;
                if (pkt->pts != AV_NOPTS_VALUE) {
                    pkt->pts += bump;
                }
            }
            if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
                pkt->pts = pkt->dts;
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                last_dts[out_idx] = pkt->dts;
                const std::int64_t step = (pkt->duration > 0) ? pkt->duration : kMinDtsStep;
                next_min_dts[out_idx] = std::max(next_min_dts[out_idx], pkt->dts + step);
            }
            pkt->pos = kUnknownPacketPos;

            if (pkt->pts != AV_NOPTS_VALUE) {
                const double end = (pkt->pts + pkt->duration) * av_q2d(dst->time_base);
                segment_end = std::max(segment_end, end);
            }

            rc = av_interleaved_write_frame(out.ctx(), pkt.get());
            av_packet_unref(pkt.get());
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Could not write a packet: " << err_str(rc);
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            if (total_sec > 0.0 && (now - last_report) >= kProgressInterval) {
                last_report = now;
                const double elapsed = std::chrono::duration<double>(now - started).count();
                const ProgressParser::Snapshot snap{
                    .current_sec = segment_end,
                    .out_time = core::format_hms(segment_end),
                    .fps = "-",
                    .speed = core::format_speed(elapsed > 0.0 ? segment_end / elapsed : 0.0),
                };
                reporter.update(snap, total_sec);
            }
        }

        // Restart every stream from at least the common wall-clock point so A/V
        // stays aligned, and strictly past its own last written DTS.
        written_sec = segment_end;
        for (const std::size_t s : std::views::iota(std::size_t{0}, offsets.size())) {
            const std::int64_t wall_offset = av_rescale_q(
                static_cast<std::int64_t>(written_sec * AV_TIME_BASE),
                AVRational{1, AV_TIME_BASE},
                out.stream(static_cast<int>(s))->time_base);
            offsets[s] = std::max(wall_offset, next_min_dts[s]);
        }
    }

    if (!out.finish()) {
        return false;
    }
    remove_partial_output.dismiss();
    return true;
}

}  // namespace

bool copy_segment(const fs::path& in_path,
                  const fs::path& out_path,
                  double start_sec,
                  double end_sec) {
    InputFile in;
    if (!in.open(in_path)) {
        return false;
    }

    // Declared before `out` so `out` closes the Windows file handle before removal on failure.
    bool created_output = false;
    auto remove_partial_output = core::make_scope_guard([&out_path, &created_output]() noexcept {
        if (created_output) {
            std::error_code ec;
            fs::remove(out_path, ec);
        }
    });

    Output out;
    if (!out.open(out_path, in.ctx(), created_output)) {
        return false;
    }

    // Seeking lands on the keyframe at or before the request because stream
    // copy cannot start mid-GOP.
    const auto seek_ts = static_cast<std::int64_t>(start_sec * AV_TIME_BASE);
    int rc = av_seek_frame(in.ctx(), kSeekAllStreams, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not seek to " << start_sec << " s: " << err_str(rc);
        return false;
    }

    detail::PacketPtr pkt = detail::make_packet();
    if (!pkt) {
        return false;
    }

    // Rebase every stream onto the first packet's DTS (falling back to PTS) so
    // each stream starts at DTS = 0 while preserving positive PTS-DTS B-frame
    // composition offsets.
    std::vector<std::int64_t> first_ts(in.ctx()->nb_streams, AV_NOPTS_VALUE);
    std::vector<std::int64_t> last_dts(out.ctx()->nb_streams, AV_NOPTS_VALUE);
    std::vector<std::uint8_t> stream_done(in.ctx()->nb_streams, kStreamActive);

    // Bound output length relative to the actual keyframe landing point so an
    // earlier keyframe does not lengthen the copied segment.
    const double wanted_sec = (end_sec >= 0.0) ? (end_sec - start_sec) : kUnsetTimeSec;
    double copy_origin = kUnsetTimeSec;

    for (;;) {
        rc = av_read_frame(in.ctx(), pkt.get());
        if (rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Read failed: " << err_str(rc);
            return false;
        }

        const int out_index = out.mapped(pkt->stream_index);
        if (out_index < 0) {
            av_packet_unref(pkt.get());
            continue;
        }

        const auto in_idx = static_cast<std::size_t>(pkt->stream_index);
        const auto out_idx = static_cast<std::size_t>(out_index);
        if (stream_done[in_idx] != kStreamActive) {
            av_packet_unref(pkt.get());
            continue;
        }

        const AVStream* src = in.ctx()->streams[in_idx];
        const std::int64_t raw_ts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
        const double pkt_sec =
            (raw_ts == AV_NOPTS_VALUE) ? 0.0 : raw_ts * av_q2d(src->time_base);
        if (copy_origin < 0.0) {
            copy_origin = pkt_sec;
        }
        if (wanted_sec >= 0.0 && (pkt_sec - copy_origin) >= wanted_sec) {
            // Stop this stream cleanly in decode order so trailing B-frames are
            // never written without their preceding reference frame.
            stream_done[in_idx] = kStreamFinished;
            av_packet_unref(pkt.get());
            continue;
        }

        if (first_ts[in_idx] == AV_NOPTS_VALUE && raw_ts != AV_NOPTS_VALUE) {
            first_ts[in_idx] = raw_ts;
        }
        if (const std::int64_t base = first_ts[in_idx]; base != AV_NOPTS_VALUE) {
            if (pkt->pts != AV_NOPTS_VALUE) {
                pkt->pts -= base;
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                pkt->dts -= base;
            }
        }

        AVStream* dst = out.stream(out_index);
        pkt->stream_index = out_index;
        av_packet_rescale_ts(pkt.get(), src->time_base, dst->time_base);
        if (pkt->dts != AV_NOPTS_VALUE && last_dts[out_idx] != AV_NOPTS_VALUE &&
            pkt->dts <= last_dts[out_idx]) {
            pkt->dts = last_dts[out_idx] + kMinDtsStep;
        }
        if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
            pkt->pts = pkt->dts;
        }
        if (pkt->dts != AV_NOPTS_VALUE) {
            last_dts[out_idx] = pkt->dts;
        }
        pkt->pos = kUnknownPacketPos;

        rc = av_interleaved_write_frame(out.ctx(), pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write a packet: " << err_str(rc);
            return false;
        }
    }

    if (!out.finish()) {
        return false;
    }
    remove_partial_output.dismiss();
    return true;
}

bool concat_copy(std::span<const fs::path> inputs,
                 const fs::path& out_path,
                 ProgressReporter& reporter,
                 double total_sec) {
    if (inputs.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Nothing to concatenate.";
        return false;
    }

    try {
        const bool ok = run_concat_copy(inputs, out_path, reporter, total_sec);
        if (ok) {
            reporter.finish();
        } else {
            reporter.abort();
        }
        return ok;
    } catch (const std::exception& e) {
        reporter.abort();
        BOOST_LOG_TRIVIAL(error) << "Concatenation failed: " << e.what();
        return false;
    } catch (...) {
        reporter.abort();
        BOOST_LOG_TRIVIAL(error) << "Concatenation failed with an unknown error.";
        return false;
    }
}

}  // namespace av
