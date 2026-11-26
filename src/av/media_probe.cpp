#include "av/media_probe.h"

#include "av/av_error.h"
#include "av/av_handles.h"
#include "core/path_util.h"
#include "core/scoped_handle.h"

#include <boost/log/trivial.hpp>

#include <exception>
#include <string>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

namespace av {

namespace {

constexpr int kAutoStreamIndex = -1;

}  // namespace

MediaInfo probe(const fs::path& file) {
    try {
        MediaInfo info;

        const std::string path_str = core::to_utf8(file);
        detail::FormatInputPtr fmt;
        int rc = avformat_open_input(core::out_ptr(fmt), path_str.c_str(), nullptr, nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(debug) << "Probe could not open " << path_str << ": " << err_str(rc);
            return info;
        }

        rc = avformat_find_stream_info(fmt.get(), nullptr);
        if (rc < 0) {
            BOOST_LOG_TRIVIAL(debug) << "Probe found no stream info in " << path_str << ": "
                                     << err_str(rc);
            return info;
        }

        info.ok = true;
        if (fmt->duration != AV_NOPTS_VALUE) {
            info.duration_sec = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        }

        const int vid = av_find_best_stream(
            fmt.get(), AVMEDIA_TYPE_VIDEO, kAutoStreamIndex, kAutoStreamIndex, nullptr, 0);
        if (vid >= 0) {
            const AVStream* st = fmt->streams[vid];
            info.has_video = true;
            info.width = st->codecpar->width;
            info.height = st->codecpar->height;

            if (const AVCodecDescriptor* desc = avcodec_descriptor_get(st->codecpar->codec_id);
                desc && desc->name) {
                info.video_codec = desc->name;
            }

            // avg_frame_rate reflects actual stream cadence; fall back to
            // r_frame_rate when captures leave avg_frame_rate unset.
            AVRational rate = st->avg_frame_rate;
            if (rate.num <= 0 || rate.den <= 0) {
                rate = st->r_frame_rate;
            }
            if (rate.num > 0 && rate.den > 0) {
                info.frame_rate = FrameRate{rate.num, rate.den};
            }
        }

        const int aud = av_find_best_stream(
            fmt.get(), AVMEDIA_TYPE_AUDIO, kAutoStreamIndex, kAutoStreamIndex, nullptr, 0);
        if (aud >= 0) {
            info.has_audio = true;
            if (const AVCodecDescriptor* desc =
                    avcodec_descriptor_get(fmt->streams[aud]->codecpar->codec_id);
                desc && desc->name) {
                info.audio_codec = desc->name;
            }
        }

        return info;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "Probe failed for " << core::to_utf8(file) << ": " << e.what();
        return {};
    } catch (...) {
        return {};
    }
}

}  // namespace av
