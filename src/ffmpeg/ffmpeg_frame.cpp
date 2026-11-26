#include "ffmpeg/ffmpeg.h"

#include "av/frame_grabber.h"

bool Ffmpeg::extract_frame(const fs::path& in, const fs::path& out_img, double ts) const {
    return av::grab_frame(in, out_img, ts);
}
