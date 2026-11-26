#include "ffmpeg/ffmpeg.h"

#include "av/frame_grabber.h"
#include "av/media_probe.h"
#include "av/remuxer.h"

double Ffmpeg::duration(const fs::path& input) const {
    return av::probe(input).duration_sec;
}

double Ffmpeg::frame_rate(const fs::path& input) const {
    return av::probe(input).frame_rate.value();
}

bool Ffmpeg::extract_frame(const fs::path& in, const fs::path& out_img, double ts) const {
    return av::grab_frame(in, out_img, ts);
}

bool Ffmpeg::run_segment(const fs::path& in, const fs::path& out,
                         double start_sec, double end_sec) const {
    return av::copy_segment(in, out, start_sec, end_sec);
}

bool Ffmpeg::concat(std::span<const fs::path> inputs, const fs::path& out,
                    double total_sec, ProgressReporter& rep) const {
    return av::concat_copy(inputs, out, rep, total_sec);
}
