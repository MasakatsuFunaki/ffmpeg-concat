#include "av/remuxer.h"

#include "av/av_error.h"
#include "ffmpeg/progress_reporter.h"

#include <boost/log/trivial.hpp>

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

namespace av {
namespace {

struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

// An output container plus the stream mapping used to write into it.
class Output {
public:
    ~Output() {
        if (!fmt_) return;
        if (fmt_->pb && !(fmt_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
    }

    // Mirror every audio/video stream of `source`, in source order.
    bool open(const fs::path& path, AVFormatContext* source) {
        int rc = avformat_alloc_output_context2(&fmt_, nullptr, nullptr, path.string().c_str());
        if (rc < 0 || !fmt_) {
            BOOST_LOG_TRIVIAL(error) << "Could not create " << path << ": " << err_str(rc);
            return false;
        }

        stream_map_.assign(source->nb_streams, -1);
        for (unsigned i = 0; i < source->nb_streams; ++i) {
            const AVStream* in = source->streams[i];
            AVMediaType type = in->codecpar->codec_type;
            if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) continue;

            AVStream* out = avformat_new_stream(fmt_, nullptr);
            if (!out) return false;
            rc = avcodec_parameters_copy(out->codecpar, in->codecpar);
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Could not copy stream parameters: " << err_str(rc);
                return false;
            }
            out->codecpar->codec_tag = 0;   // let the muxer pick a tag it supports
            out->time_base = in->time_base;
            stream_map_[i] = out->index;
        }

        if (!(fmt_->oformat->flags & AVFMT_NOFILE)) {
            rc = avio_open(&fmt_->pb, path.string().c_str(), AVIO_FLAG_WRITE);
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Could not create " << path << ": " << err_str(rc);
                return false;
            }
        }
        rc = avformat_write_header(fmt_, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write the container header: " << err_str(rc);
            return false;
        }
        return true;
    }

    int mapped(int source_index) const {
        return source_index < static_cast<int>(stream_map_.size()) ? stream_map_[source_index] : -1;
    }
    AVStream* stream(int out_index) const { return fmt_->streams[out_index]; }
    AVFormatContext* ctx() const { return fmt_; }

    bool finish() {
        int rc = av_write_trailer(fmt_);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not finalize the output: " << err_str(rc);
            return false;
        }
        return true;
    }

private:
    AVFormatContext* fmt_ = nullptr;
    std::vector<int> stream_map_;
};

struct InputFile {
    AVFormatContext* fmt = nullptr;
    ~InputFile() { if (fmt) avformat_close_input(&fmt); }

    bool open(const fs::path& path) {
        int rc = avformat_open_input(&fmt, path.string().c_str(), nullptr, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not open " << path << ": " << err_str(rc);
            return false;
        }
        rc = avformat_find_stream_info(fmt, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "No stream info in " << path << ": " << err_str(rc);
            return false;
        }
        return true;
    }
};

std::string format_hms(double seconds) {
    if (seconds < 0) seconds = 0;
    int total = static_cast<int>(seconds);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << (total / 3600) << ":"
        << std::setfill('0') << std::setw(2) << ((total % 3600) / 60) << ":"
        << std::setfill('0') << std::setw(2) << (total % 60);
    return oss.str();
}

}  // namespace

bool copy_segment(const fs::path& in_path, const fs::path& out_path,
                  double start_sec, double end_sec) {
    InputFile in;
    if (!in.open(in_path)) return false;

    Output out;
    if (!out.open(out_path, in.fmt)) return false;

    // Seeking lands on the keyframe at or before the request, which is the same
    // approximation input seeking gave: a stream copy cannot start mid-GOP.
    int64_t seek_ts = static_cast<int64_t>(start_sec * AV_TIME_BASE);
    int rc = av_seek_frame(in.fmt, -1, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(error) << "Could not seek to " << start_sec << " s: " << err_str(rc);
        return false;
    }

    PacketPtr pkt(av_packet_alloc());
    if (!pkt) return false;

    // Rebase every stream onto the first packet's DTS (falling back to PTS) so
    // each stream starts at DTS = 0 while preserving positive PTS-DTS B-frame
    // composition offsets.
    std::vector<int64_t> first_ts(in.fmt->nb_streams, AV_NOPTS_VALUE);
    std::vector<int64_t> last_dts(out.ctx()->nb_streams, AV_NOPTS_VALUE);
    std::vector<bool> stream_done(in.fmt->nb_streams, false);

    // The seek lands on a keyframe at or before start_sec, so the copy really
    // begins somewhere earlier. Bound the result by how much content to keep
    // from that landing point rather than by an absolute end time — otherwise a
    // segment that seeks back a second comes out a second too long.
    const double wanted_sec = (end_sec >= 0) ? end_sec - start_sec : -1.0;
    double copy_origin = -1.0;

    for (;;) {
        rc = av_read_frame(in.fmt, pkt.get());
        if (rc == AVERROR_EOF) break;
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Read failed: " << err_str(rc);
            return false;
        }

        int out_index = out.mapped(pkt->stream_index);
        if (out_index < 0 || stream_done[pkt->stream_index]) {
            av_packet_unref(pkt.get());
            continue;
        }

        const AVStream* src = in.fmt->streams[pkt->stream_index];
        int64_t raw_ts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
        double pkt_sec = (raw_ts == AV_NOPTS_VALUE) ? 0.0 : raw_ts * av_q2d(src->time_base);
        if (copy_origin < 0) copy_origin = pkt_sec;
        if (wanted_sec >= 0 && pkt_sec - copy_origin >= wanted_sec) {
            // Stop this stream cleanly in decode order so trailing B-frames are
            // never written without their preceding reference frame.
            stream_done[pkt->stream_index] = true;
            av_packet_unref(pkt.get());
            continue;
        }

        if (first_ts[pkt->stream_index] == AV_NOPTS_VALUE && raw_ts != AV_NOPTS_VALUE) {
            first_ts[pkt->stream_index] = raw_ts;
        }
        int64_t base = first_ts[pkt->stream_index];
        if (base != AV_NOPTS_VALUE) {
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= base;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= base;
        }

        AVStream* dst = out.stream(out_index);
        pkt->stream_index = out_index;
        av_packet_rescale_ts(pkt.get(), src->time_base, dst->time_base);
        if (pkt->dts != AV_NOPTS_VALUE && last_dts[out_index] != AV_NOPTS_VALUE &&
            pkt->dts <= last_dts[out_index]) {
            pkt->dts = last_dts[out_index] + 1;
        }
        if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
            pkt->pts = pkt->dts;
        }
        if (pkt->dts != AV_NOPTS_VALUE) {
            last_dts[out_index] = pkt->dts;
        }
        pkt->pos = -1;

        rc = av_interleaved_write_frame(out.ctx(), pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(error) << "Could not write a packet: " << err_str(rc);
            return false;
        }
    }

    return out.finish();
}

bool concat_copy(const std::vector<fs::path>& inputs, const fs::path& out_path,
                 ProgressReporter& reporter, double total_sec) {
    if (inputs.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Nothing to concatenate.";
        return false;
    }

    InputFile first;
    if (!first.open(inputs.front())) return false;

    Output out;
    if (!out.open(out_path, first.fmt)) return false;

    auto started = std::chrono::steady_clock::now();
    auto last_report = started;
    double written_sec = 0.0;      // end of everything written so far
    const size_t num_out_streams = out.ctx()->nb_streams;
    std::vector<int64_t> offsets(num_out_streams, 0);
    std::vector<int64_t> last_dts(num_out_streams, AV_NOPTS_VALUE);
    std::vector<int64_t> next_min_dts(num_out_streams, 0);

    PacketPtr pkt(av_packet_alloc());
    if (!pkt) return false;

    bool ok = true;
    for (size_t i = 0; i < inputs.size() && ok; ++i) {
        InputFile current;
        InputFile* source = &current;
        if (i == 0) {
            source = &first;       // already open
        } else if (!current.open(inputs[i])) {
            return false;
        }

        std::vector<int64_t> first_ts(source->fmt->nb_streams, AV_NOPTS_VALUE);
        double segment_end = written_sec;

        for (;;) {
            int rc = av_read_frame(source->fmt, pkt.get());
            if (rc == AVERROR_EOF) break;
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Read failed: " << err_str(rc);
                return false;
            }

            int out_index = out.mapped(pkt->stream_index);
            if (out_index < 0) {
                av_packet_unref(pkt.get());
                continue;
            }

            const AVStream* src = source->fmt->streams[pkt->stream_index];
            AVStream* dst = out.stream(out_index);

            int64_t raw_ts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
            if (first_ts[pkt->stream_index] == AV_NOPTS_VALUE && raw_ts != AV_NOPTS_VALUE) {
                first_ts[pkt->stream_index] = raw_ts;
            }
            int64_t base = first_ts[pkt->stream_index];
            if (base != AV_NOPTS_VALUE) {
                if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= base;
                if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= base;
            }

            pkt->stream_index = out_index;
            av_packet_rescale_ts(pkt.get(), src->time_base, dst->time_base);
            // Shift this segment to start where the previous one ended.
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += offsets[out_index];
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += offsets[out_index];
            if (pkt->dts != AV_NOPTS_VALUE && last_dts[out_index] != AV_NOPTS_VALUE &&
                pkt->dts <= last_dts[out_index]) {
                int64_t bump = (last_dts[out_index] + 1) - pkt->dts;
                pkt->dts += bump;
                if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += bump;
            }
            if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
                pkt->pts = pkt->dts;
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                last_dts[out_index] = pkt->dts;
                int64_t step = (pkt->duration > 0) ? pkt->duration : 1;
                if (pkt->dts + step > next_min_dts[out_index]) {
                    next_min_dts[out_index] = pkt->dts + step;
                }
            }
            pkt->pos = -1;

            if (pkt->pts != AV_NOPTS_VALUE) {
                double end = (pkt->pts + pkt->duration) * av_q2d(dst->time_base);
                if (end > segment_end) segment_end = end;
            }

            rc = av_interleaved_write_frame(out.ctx(), pkt.get());
            av_packet_unref(pkt.get());
            if (rc < 0) {
                BOOST_LOG_TRIVIAL(error) << "Could not write a packet: " << err_str(rc);
                return false;
            }

            auto now = std::chrono::steady_clock::now();
            if (total_sec > 0 && now - last_report >= std::chrono::milliseconds(200)) {
                last_report = now;
                double elapsed = std::chrono::duration<double>(now - started).count();
                ProgressParser::Snapshot snap;
                snap.current_sec = segment_end;
                snap.out_time = format_hms(segment_end);
                snap.fps = "-";
                char speed[32];
                std::snprintf(speed, sizeof(speed), "%.3gx",
                              elapsed > 0 ? segment_end / elapsed : 0.0);
                snap.speed = speed;
                reporter.update(snap, total_sec);
            }
        }

        // Every stream restarts from at least the common wall-clock point so A/V
        // stays aligned, and strictly past its own last written DTS.
        written_sec = segment_end;
        for (size_t s = 0; s < offsets.size(); ++s) {
            int64_t wall_offset = av_rescale_q(static_cast<int64_t>(written_sec * AV_TIME_BASE),
                                               AVRational{1, AV_TIME_BASE},
                                               out.stream(static_cast<int>(s))->time_base);
            offsets[s] = std::max(wall_offset, next_min_dts[s]);
        }
    }

    ok = out.finish();
    if (ok) {
        reporter.finish();
    } else {
        reporter.abort();
    }
    return ok;
}

}  // namespace av
