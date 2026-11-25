#include <gtest/gtest.h>
#include "encode/encode_plan.h"

#include <algorithm>
#include <string>
#include <vector>

// ---------- helpers ----------

using Options = std::vector<std::pair<std::string, std::string>>;

static std::string option_value(const Options& opts, const std::string& key) {
    for (const auto& kv : opts) {
        if (kv.first == key) return kv.second;
    }
    return "";
}

static bool has_option(const Options& opts, const std::string& key) {
    return std::any_of(opts.begin(), opts.end(),
                       [&](const auto& kv) { return kv.first == key; });
}

static EncodePlan two_input_plan() {
    EncodePlan plan;
    plan.inputs  = {"first.mp4", "second.mp4"};
    plan.output  = "out.mp4";
    plan.encoder = "libx264";
    plan.crf     = 30;
    plan.preset  = "ultrafast";
    return plan;
}

// ---------- lowest_fps_if_mixed ----------

static const av::FrameRate FPS_60{60, 1};
static const av::FrameRate FPS_50{50, 1};
static const av::FrameRate FPS_30{30, 1};
static const av::FrameRate FPS_24{24, 1};
static const av::FrameRate FPS_2997{30000, 1001};
static const av::FrameRate UNPROBED{};

TEST(LowestFpsIfMixed, PicksLowestWhenRatesDiffer) {
    EXPECT_DOUBLE_EQ(30.0, lowest_fps_if_mixed({FPS_30, FPS_60}).value());
}

TEST(LowestFpsIfMixed, IsOrderIndependent) {
    // The lowest rate must win even when the highest is the first segment.
    EXPECT_DOUBLE_EQ(30.0, lowest_fps_if_mixed({FPS_60, FPS_30}).value());
    EXPECT_DOUBLE_EQ(24.0, lowest_fps_if_mixed({FPS_60, FPS_30, FPS_24, FPS_50}).value());
}

TEST(LowestFpsIfMixed, ReturnsUnknownWhenRatesAgree) {
    EXPECT_FALSE(lowest_fps_if_mixed({FPS_30, FPS_30, FPS_30}).known());
}

TEST(LowestFpsIfMixed, ToleratesRoundingNoise) {
    // 30000/1001 and 29.97 are the same rate; that is not a mix worth resampling.
    EXPECT_FALSE(lowest_fps_if_mixed({FPS_2997, av::FrameRate{2997, 100}}).known());
    // ...but a genuine 29.97 vs 30 difference is.
    EXPECT_TRUE(lowest_fps_if_mixed({FPS_30, FPS_2997}).known());
    EXPECT_NEAR(29.97, lowest_fps_if_mixed({FPS_30, FPS_2997}).value(), 0.001);
}

TEST(LowestFpsIfMixed, PreservesTheExactRational) {
    // 30000/1001 must survive as a rational; rounding it to 29.97 would drift.
    av::FrameRate picked = lowest_fps_if_mixed({FPS_60, FPS_2997});
    EXPECT_EQ(30000, picked.num);
    EXPECT_EQ(1001, picked.den);
}

TEST(LowestFpsIfMixed, NeedsTwoKnownRates) {
    EXPECT_FALSE(lowest_fps_if_mixed({}).known());
    EXPECT_FALSE(lowest_fps_if_mixed({FPS_60}).known());
    EXPECT_FALSE(lowest_fps_if_mixed({UNPROBED, FPS_60}).known());  // one probe failed
}

TEST(LowestFpsIfMixed, IgnoresUnprobedEntries) {
    // A file whose rate could not be read must not be mistaken for 0 fps.
    EXPECT_DOUBLE_EQ(30.0, lowest_fps_if_mixed({UNPROBED, FPS_60, FPS_30}).value());
}

// ---------- build_filter_description ----------

TEST(BuildFilterDescription, SingleInputPassesThrough) {
    EncodePlan plan;
    plan.inputs  = {"only.mp4"};
    plan.output  = "out.mp4";
    plan.encoder = "libx264";

    // Still a graph, because the engine always reads from named pads.
    EXPECT_EQ("[in0v]null[outv];[in0a]anull[outa]", build_filter_description(plan));
}

TEST(BuildFilterDescription, SingleInputStillHonoursATargetRate) {
    EncodePlan plan;
    plan.inputs     = {"only.mp4"};
    plan.output     = "out.mp4";
    plan.encoder    = "libx264";
    plan.target_fps = FPS_30;

    EXPECT_EQ("[in0v]fps=30/1[outv];[in0a]anull[outa]", build_filter_description(plan));
}

TEST(BuildFilterDescription, PlainConcatWhenNoNormalizationNeeded) {
    std::string filter = build_filter_description(two_input_plan());

    EXPECT_EQ("[in0v][in0a][in1v][in1a]concat=n=2:v=1:a=1[outv][outa]", filter);
    EXPECT_EQ(std::string::npos, filter.find("fps="));
}

TEST(BuildFilterDescription, NormalizesEveryInputAheadOfConcat) {
    EncodePlan plan = two_input_plan();
    plan.target_fps = FPS_30;

    // Both segments are rate-converted before the concat sees them — including
    // the one already at the target rate, so the join is uniform either way.
    EXPECT_EQ("[in0v]fps=30/1[v0];[in1v]fps=30/1[v1];"
              "[v0][in0a][v1][in1a]concat=n=2:v=1:a=1[outv][outa]",
              build_filter_description(plan));
}

TEST(BuildFilterDescription, ScalesToAnyInputCount) {
    EncodePlan plan;
    plan.inputs     = {"a.mp4", "b.mp4", "c.mp4"};
    plan.output     = "out.mp4";
    plan.encoder    = "libx264";
    plan.target_fps = FPS_24;

    EXPECT_EQ("[in0v]fps=24/1[v0];[in1v]fps=24/1[v1];[in2v]fps=24/1[v2];"
              "[v0][in0a][v1][in1a][v2][in2a]concat=n=3:v=1:a=1[outv][outa]",
              build_filter_description(plan));
}

TEST(BuildFilterDescription, EmitsRatesAsExactRationals) {
    // Never a decimal: libavfilter parses numbers through the C locale, which a
    // library in the process can switch to comma-decimal at any time. An NTSC
    // rate must also survive exactly rather than round to 29.97.
    EncodePlan plan = two_input_plan();
    plan.target_fps = FPS_2997;

    std::string filter = build_filter_description(plan);

    EXPECT_NE(std::string::npos, filter.find("fps=30000/1001"));
    EXPECT_EQ(std::string::npos, filter.find("."));
    EXPECT_EQ(std::string::npos, filter.find(","));
}

// ---------- build_request ----------

TEST(BuildRequest, CarriesThePlanThrough) {
    EncodePlan plan = two_input_plan();
    plan.hwaccel_cuda = true;
    plan.target_fps   = FPS_30;

    av::TranscodeRequest req = build_request(plan);

    EXPECT_EQ(plan.inputs, req.inputs);
    EXPECT_EQ(plan.output, req.output);
    EXPECT_EQ("libx264", req.video_encoder);
    EXPECT_TRUE(req.hwaccel_cuda);
    EXPECT_EQ(build_filter_description(plan), req.filter_description);
    EXPECT_EQ("aac", req.audio_encoder);
    EXPECT_EQ(192000, req.audio_bit_rate);
}

TEST(BuildRequest, OmitsHwaccelByDefault) {
    EXPECT_FALSE(build_request(two_input_plan()).hwaccel_cuda);
}

TEST(BuildRequest, UsesTheNvencDialect) {
    EncodePlan plan = two_input_plan();
    plan.encoder = "av1_nvenc";
    plan.crf     = 24;
    plan.preset  = "p6";

    av::TranscodeRequest req = build_request(plan);

    EXPECT_EQ("av1_nvenc", req.video_encoder);
    EXPECT_EQ("24", option_value(req.video_options, "cq"));   // nvenc says cq, not crf
    EXPECT_EQ("p6", option_value(req.video_options, "preset"));
    EXPECT_FALSE(has_option(req.video_options, "crf"));
    EXPECT_EQ(0, req.video_bit_rate);                          // quality-targeted
}

TEST(BuildRequest, UsesTheX264Dialect) {
    av::TranscodeRequest req = build_request(two_input_plan());

    EXPECT_EQ("30", option_value(req.video_options, "crf"));
    EXPECT_EQ("ultrafast", option_value(req.video_options, "preset"));
    EXPECT_FALSE(has_option(req.video_options, "cq"));
}

TEST(BuildRequest, UsesTheLibaomDialect) {
    EncodePlan plan = two_input_plan();
    plan.encoder = "libaom-av1";
    plan.preset  = "";        // falls back to the profile default

    av::TranscodeRequest req = build_request(plan);

    EXPECT_EQ("30", option_value(req.video_options, "crf"));
    EXPECT_EQ("4", option_value(req.video_options, "cpu-used"));  // libaom's "preset"
    EXPECT_FALSE(has_option(req.video_options, "preset"));
}

TEST(BuildRequest, FallsBackToLibaomForUnknownEncoders) {
    EncodePlan plan = two_input_plan();
    plan.encoder = "some_future_encoder";

    av::TranscodeRequest req = build_request(plan);

    EXPECT_EQ("some_future_encoder", req.video_encoder);
    EXPECT_TRUE(has_option(req.video_options, "cpu-used"));
}
