#include "av/media_probe.h"

#include "av/av_error.h"

#include <boost/log/trivial.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

namespace av {

MediaInfo probe(const fs::path& file) {
    MediaInfo info;

    AVFormatContext* fmt = nullptr;
    int rc = avformat_open_input(&fmt, file.string().c_str(), nullptr, nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(debug) << "Probe could not open " << file << ": " << err_str(rc);
        return info;
    }

    rc = avformat_find_stream_info(fmt, nullptr);
    if (rc < 0) {
        BOOST_LOG_TRIVIAL(debug) << "Probe found no stream info in " << file << ": " << err_str(rc);
        avformat_close_input(&fmt);
        return info;
    }

    info.ok = true;
    if (fmt->duration != AV_NOPTS_VALUE) {
        info.duration_sec = static_cast<double>(fmt->duration) / AV_TIME_BASE;
    }

    int vid = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vid >= 0) {
        const AVStream* st = fmt->streams[vid];
        info.has_video = true;
        info.width  = st->codecpar->width;
        info.height = st->codecpar->height;

        const AVCodecDescriptor* desc = avcodec_descriptor_get(st->codecpar->codec_id);
        if (desc && desc->name) info.video_codec = desc->name;

        // avg_frame_rate is what the file actually runs at; r_frame_rate is only
        // the finest tick the container can express. Fall back to r_frame_rate
        // when the average is missing (some captures leave it unset).
        AVRational rate = st->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0) rate = st->r_frame_rate;
        if (rate.num > 0 && rate.den > 0) info.frame_rate = FrameRate{rate.num, rate.den};
    }

    int aud = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aud >= 0) {
        info.has_audio = true;
        const AVCodecDescriptor* desc = avcodec_descriptor_get(fmt->streams[aud]->codecpar->codec_id);
        if (desc && desc->name) info.audio_codec = desc->name;
    }

    avformat_close_input(&fmt);
    return info;
}

}  // namespace av
