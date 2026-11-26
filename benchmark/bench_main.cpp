// ---------------------------------------------------------------------------
// benchmark/bench_main.cpp – Google Benchmark suite for ffmpeg-tools core,
// libav*, and extract pipeline bottlenecks (BEFORE vs AFTER comparison).
//
// All disk scratch work runs inside a self-cleaning RAII temporary directory
// under std::filesystem::temp_directory_path() so committed fixtures under
// BENCHMARK_DATA_DIR remain strictly read-only.
// ---------------------------------------------------------------------------

#include "av/frame_grabber.h"
#include "av/frame_sampler.h"
#include "av/media_probe.h"
#include "core/path_util.h"
#include "core/profiler.h"
#include "extract/crop_deduplicator.h"
#include "extract/equally_spaced_extractor.h"
#include "extract/fingerprint_cache.h"
#include "extract/frame_scorer.h"
#include "extract/image_io.h"
#include "extract/mosaic_builder.h"
#include "extract/striking_frame_extractor.h"
#include "extract/text_crop_extractor.h"
#include "extract/text_region_detector.h"
#include "ffmpeg/ffmpeg.h"
#include "media/media_fs.h"

#include <benchmark/benchmark.h>

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kMainVideoRel = "2026-03-22 00-16-33.mp4";
constexpr std::string_view kUniformClipRel = "fps_uniform/2026-01-01 00-00-00.mp4";

class BenchTempDir {
public:
    explicit BenchTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        std::mt19937_64 rng{std::random_device{}()};
        const fs::path base = fs::temp_directory_path();
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());

        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
            const auto salt = rng() & 0xFFFF'FFFFULL;
            fs::path candidate =
                base / std::format("fftools_bench_{}_{:x}_{}_{:08x}", prefix, tick, seq, salt);
            std::error_code ec;
            if (fs::create_directories(candidate, ec) && !ec) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("BenchTempDir: failed to create temporary directory");
    }

    ~BenchTempDir() noexcept { cleanup(); }

    BenchTempDir(const BenchTempDir&) = delete;
    BenchTempDir& operator=(const BenchTempDir&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    void cleanup() noexcept {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        fs::remove_all(path_, ec);
        path_.clear();
    }

    fs::path path_;
};

[[nodiscard]] fs::path fixture_path(std::string_view rel) {
    return fs::path(BENCHMARK_DATA_DIR) / fs::path(rel);
}

// Synthesizes a visually distinct 640x360 BGR image keyed by `seed` so
// deduplication benchmarks hit realistic image dimensions and non-trivial
// correlation scores without collapsing to identical frames.
[[nodiscard]] cv::Mat make_distinct_crop(int seed, int width = 640, int height = 360) {
    cv::Mat img(height, width, CV_8UC3, cv::Scalar(30, 30, 30));
    const int freq_x = 8 + (seed % 23) * 3;
    const int freq_y = 8 + ((seed / 23) % 19) * 4;
    const int phase = (seed * 37) % 128;

    for (int r = 0; r < height; r += 8) {
        const int y2 = std::min(height, r + 8);
        for (int c = 0; c < width; c += 8) {
            const int x2 = std::min(width, c + 8);
            const int wave = ((c + phase) / freq_x + (r * 3 + seed * 11) / freq_y) % 4;
            const auto b = static_cast<unsigned char>((wave * 61 + seed * 17) & 0xFF);
            const auto g = static_cast<unsigned char>((wave * 97 + seed * 29) & 0xFF);
            const auto r_ch = static_cast<unsigned char>((wave * 131 + seed * 43) & 0xFF);
            cv::rectangle(img, cv::Rect(c, r, x2 - c, y2 - r), cv::Scalar(b, g, r_ch), cv::FILLED);
        }
    }
    cv::putText(img, std::format("SEED {}", seed), cv::Point(20 + (seed % 180), 80 + (seed % 200)),
                cv::FONT_HERSHEY_SIMPLEX, 1.4, cv::Scalar(240, 240, 240), 3, cv::LINE_AA);
    return img;
}

[[nodiscard]] cv::Mat make_synthetic_hd_text_frame() {
    cv::Mat frame(1080, 1920, CV_8UC3, cv::Scalar(235, 235, 240));
    const std::array<std::string_view, 4> lines{{
        "FFMPEG CODEC TOOLS BENCHMARK 2026",
        "PERFORMANCE BASELINE MEASUREMENT",
        "CHAPTER 04: IN-MEMORY FRAME SAMPLING",
        "SUBTITLE: SHARP HIGH CONTRAST TEXT",
    }};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const int y = 180 + static_cast<int>(i) * 220;
        cv::rectangle(frame, cv::Rect(120, y - 65, 1680, 95), cv::Scalar(248, 248, 248), cv::FILLED);
        cv::putText(frame, std::string(lines[i]), cv::Point(150, y),
                    cv::FONT_HERSHEY_SIMPLEX, 1.8, cv::Scalar(18, 18, 22), 3, cv::LINE_AA);
    }
    return frame;
}

// ---------------------------------------------------------------------------
// Core & Media microbenchmarks
// ---------------------------------------------------------------------------

void BM_Profiler_DisabledScopedOverhead(benchmark::State& state) {
    core::Profiler profiler(false);
    for ([[maybe_unused]] auto _ : state) {
        core::ScopedProfile scope("hot.stage", profiler);
        bool active = scope.is_active();
        benchmark::DoNotOptimize(active);
    }
}
BENCHMARK(BM_Profiler_DisabledScopedOverhead);

void BM_Profiler_EnabledScopedRecord(benchmark::State& state) {
    core::Profiler profiler(true);
    for ([[maybe_unused]] auto _ : state) {
        core::ScopedProfile scope("hot.stage", profiler);
        benchmark::ClobberMemory();
    }
    auto stats = profiler.stage_stats("hot.stage");
    benchmark::DoNotOptimize(stats);
}
BENCHMARK(BM_Profiler_EnabledScopedRecord);

void BM_MediaFileDate(benchmark::State& state) {
    const fs::path video = fixture_path(kMainVideoRel);
    for ([[maybe_unused]] auto _ : state) {
        std::time_t ts = media::file_date(video);
        benchmark::DoNotOptimize(ts);
    }
}
BENCHMARK(BM_MediaFileDate);

// ---------------------------------------------------------------------------
// libav* container probe & frame extraction benchmarks
// ---------------------------------------------------------------------------

void BM_AvProbe_MainVideo(benchmark::State& state) {
    const fs::path video = fixture_path(kMainVideoRel);
    if (!fs::is_regular_file(video)) {
        state.SkipWithError("Missing main video fixture");
        return;
    }
    for ([[maybe_unused]] auto _ : state) {
        const av::MediaInfo info = av::probe(video);
        double dur = info.duration_sec;
        benchmark::DoNotOptimize(dur);
    }
}
BENCHMARK(BM_AvProbe_MainVideo)->Unit(benchmark::kMillisecond);

void BM_AvProbe_UniformClip(benchmark::State& state) {
    const fs::path video = fixture_path(kUniformClipRel);
    if (!fs::is_regular_file(video)) {
        state.SkipWithError("Missing uniform clip fixture");
        return;
    }
    for ([[maybe_unused]] auto _ : state) {
        const av::MediaInfo info = av::probe(video);
        double dur = info.duration_sec;
        benchmark::DoNotOptimize(dur);
    }
}
BENCHMARK(BM_AvProbe_UniformClip)->Unit(benchmark::kMillisecond);

void BM_AvGrabFrame_Multi10(benchmark::State& state) {
    const fs::path video = fixture_path(kMainVideoRel);
    const av::MediaInfo info = av::probe(video);
    if (!info.ok || info.duration_sec <= 0.0) {
        state.SkipWithError("Could not probe main video fixture");
        return;
    }

    BenchTempDir scratch("grab_multi10");
    constexpr int kFrameCount = 10;
    const double step = info.duration_sec / static_cast<double>(kFrameCount);

    for ([[maybe_unused]] auto _ : state) {
        int decoded = 0;
        for (int i = 0; i < kFrameCount; ++i) {
            const double ts = (static_cast<double>(i) + 0.5) * step;
            const fs::path out_jpg = scratch.path() / std::format("f_{:04d}.jpg", i);
            if (av::grab_frame(video, out_jpg, ts)) {
                const cv::Mat img = extract_io::read_image(out_jpg);
                if (!img.empty()) {
                    ++decoded;
                }
            }
        }
        benchmark::DoNotOptimize(decoded);
    }
    state.SetItemsProcessed(state.iterations() * kFrameCount);
}
BENCHMARK(BM_AvGrabFrame_Multi10)->Unit(benchmark::kMillisecond)->MinTime(0.2);

void BM_AvFrameSampler_Multi10(benchmark::State& state) {
    const fs::path video = fixture_path(kMainVideoRel);
    const av::MediaInfo info = av::probe(video);
    if (!info.ok || info.duration_sec <= 0.0) {
        state.SkipWithError("Could not probe main video fixture");
        return;
    }

    constexpr int kFrameCount = 10;
    const double step = info.duration_sec / static_cast<double>(kFrameCount);
    std::array<double, kFrameCount> timestamps{};
    for (int i = 0; i < kFrameCount; ++i) {
        timestamps[static_cast<std::size_t>(i)] = (static_cast<double>(i) + 0.5) * step;
    }

    for ([[maybe_unused]] auto _ : state) {
        av::FrameSampler sampler;
        int decoded = 0;
        if (sampler.open(video)) {
            for (const double ts : timestamps) {
                if (const auto frame = sampler.sample_at(ts); frame && !frame->empty()) {
                    const cv::Mat view = extract_io::as_mat_view(*frame);
                    if (!view.empty()) {
                        ++decoded;
                    }
                }
            }
        }
        benchmark::DoNotOptimize(decoded);
    }
    state.SetItemsProcessed(state.iterations() * kFrameCount);
}
BENCHMARK(BM_AvFrameSampler_Multi10)->Unit(benchmark::kMillisecond)->MinTime(0.2);

// ---------------------------------------------------------------------------
// Extract component microbenchmarks
// ---------------------------------------------------------------------------

// Stateless is_unique baseline: O(N) full-res resize + cvtColor + matchTemplate per check.
void BM_CropDeduplicator_CheckOne_Stateless(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    std::vector<cv::Mat> saved;
    saved.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        saved.push_back(make_distinct_crop(i));
    }
    const cv::Mat candidate = make_distinct_crop(n + 1000);
    const CropDeduplicator dedup(0.85);

    for ([[maybe_unused]] auto _ : state) {
        bool unique = dedup.is_unique(candidate, std::span<const cv::Mat>{saved});
        benchmark::DoNotOptimize(unique);
    }
    state.SetComplexityN(n);
}
BENCHMARK(BM_CropDeduplicator_CheckOne_Stateless)
    ->Arg(10)
    ->Arg(50)
    ->Arg(200)
    ->Arg(500)
    ->Unit(benchmark::kMillisecond)
    ->MinTime(0.1);

// Cached-thumbnail API: precomputed normalized thumbnails + contiguous SIMD dot product.
void BM_CropDeduplicator_CheckOne_Cached(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    CropDeduplicator dedup(0.85);
    for (int i = 0; i < n; ++i) {
        (void)dedup.try_add(make_distinct_crop(i));
    }
    const cv::Mat candidate = make_distinct_crop(n + 1000);

    for ([[maybe_unused]] auto _ : state) {
        bool unique = dedup.is_unique_against_cached(candidate);
        benchmark::DoNotOptimize(unique);
    }
    state.SetComplexityN(n);
}
BENCHMARK(BM_CropDeduplicator_CheckOne_Cached)
    ->Arg(10)
    ->Arg(50)
    ->Arg(200)
    ->Arg(500)
    ->Unit(benchmark::kMillisecond)
    ->MinTime(0.1);

void BM_FrameScorer_FromDisk(benchmark::State& state) {
    const fs::path video = fixture_path(kMainVideoRel);
    BenchTempDir scratch("scorer_disk");
    const fs::path frame_jpg = scratch.path() / "sample.jpg";

    if (!av::grab_frame(video, frame_jpg, 1.0)) {
        state.SkipWithError("Failed to grab sample frame for FrameScorer benchmark");
        return;
    }

    const FrameScorer scorer;
    for ([[maybe_unused]] auto _ : state) {
        double score = scorer.score(frame_jpg);
        benchmark::DoNotOptimize(score);
    }
}
BENCHMARK(BM_FrameScorer_FromDisk)->Unit(benchmark::kMillisecond);

void BM_FrameScorer_InMemory(benchmark::State& state) {
    const fs::path video = fixture_path(kMainVideoRel);
    av::FrameSampler sampler;
    if (!sampler.open(video)) {
        state.SkipWithError("Failed to open video for in-memory FrameScorer benchmark");
        return;
    }
    const auto fb = sampler.sample_at(1.0);
    if (!fb || fb->empty()) {
        state.SkipWithError("Failed to sample frame for in-memory FrameScorer benchmark");
        return;
    }
    const cv::Mat frame = extract_io::as_mat_view(*fb).clone();

    const FrameScorer scorer;
    for ([[maybe_unused]] auto _ : state) {
        double score = scorer.score(frame);
        benchmark::DoNotOptimize(score);
    }
}
BENCHMARK(BM_FrameScorer_InMemory)->Unit(benchmark::kMillisecond);

void BM_TextRegionDetector_HD(benchmark::State& state) {
    const cv::Mat frame = make_synthetic_hd_text_frame();
    const TextRegionDetector detector;

    for ([[maybe_unused]] auto _ : state) {
        const auto rects = detector.detect(frame);
        std::size_t count = rects.size();
        benchmark::DoNotOptimize(count);
    }
}
BENCHMARK(BM_TextRegionDetector_HD)->Unit(benchmark::kMillisecond);

void BM_MosaicBuilder_6Tiles(benchmark::State& state) {
    BenchTempDir scratch("mosaic");
    const fs::path detect_dir = scratch.path() / "detect";
    fs::create_directories(detect_dir);

    for (int i = 1; i <= 6; ++i) {
        const cv::Mat tile = make_distinct_crop(i * 13, 1920, 1080);
        const fs::path path = detect_dir / std::format("striking_{:04d}.jpg", i);
        (void)extract_io::write_jpeg(path, tile);
    }

    const MosaicBuilder builder;
    int run_idx = 0;
    for ([[maybe_unused]] auto _ : state) {
        const fs::path mosaic_dir = scratch.path() / std::format("out_{}", run_idx++);
        bool ok = builder.build(detect_dir, mosaic_dir, 6, 1);
        benchmark::DoNotOptimize(ok);
        state.PauseTiming();
        std::error_code ec;
        fs::remove_all(mosaic_dir, ec);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_MosaicBuilder_6Tiles)->Unit(benchmark::kMillisecond)->MinTime(0.2);

void BM_FingerprintCache_Dir20(benchmark::State& state) {
    BenchTempDir scratch("fp_cache");
    const fs::path img_dir = scratch.path() / "imgs";
    fs::create_directories(img_dir);

    for (int i = 1; i <= 20; ++i) {
        const cv::Mat img = make_distinct_crop(i, 640, 360);
        const fs::path path = img_dir / std::format("striking_{:04d}.jpg", i);
        (void)extract_io::write_jpeg(path, img);
    }

    for ([[maybe_unused]] auto _ : state) {
        const std::string fp = FingerprintCache::compute_dir_fingerprint(img_dir, "striking_");
        std::size_t len = fp.size();
        benchmark::DoNotOptimize(len);
    }
}
BENCHMARK(BM_FingerprintCache_Dir20)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
// End-to-end Extract pipeline benchmarks (main 1080p HEVC fixture)
// ---------------------------------------------------------------------------

void BM_EquallySpacedExtractor_10Frames(benchmark::State& state) {
    const fs::path video = fixture_path(kMainVideoRel);
    if (!fs::is_regular_file(video)) {
        state.SkipWithError("Missing main video fixture");
        return;
    }

    BenchTempDir scratch("eq_spaced");
    const Ffmpeg ff;
    int run_idx = 0;

    for ([[maybe_unused]] auto _ : state) {
        const fs::path out_dir = scratch.path() / std::format("run_{}", run_idx++);
        EquallySpacedExtractor extractor(10);
        bool ok = extractor.run(ff, video, out_dir);
        benchmark::DoNotOptimize(ok);
        state.PauseTiming();
        std::error_code ec;
        fs::remove_all(out_dir, ec);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_EquallySpacedExtractor_10Frames)->Unit(benchmark::kMillisecond)->MinTime(0.2);

void BM_StrikingFrameExtractor_MainVideo(benchmark::State& state) {
    const fs::path video = fixture_path(kMainVideoRel);
    if (!fs::is_regular_file(video)) {
        state.SkipWithError("Missing main video fixture");
        return;
    }

    BenchTempDir scratch("striking");
    const Ffmpeg ff;
    int run_idx = 0;

    for ([[maybe_unused]] auto _ : state) {
        const fs::path out_dir = scratch.path() / std::format("run_{}", run_idx++);
        StrikingFrameExtractor extractor(std::nullopt);
        bool ok = extractor.run(ff, video, out_dir);
        benchmark::DoNotOptimize(ok);
        state.PauseTiming();
        std::error_code ec;
        fs::remove_all(out_dir, ec);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_StrikingFrameExtractor_MainVideo)->Unit(benchmark::kMillisecond)->Iterations(2);

void BM_TextCropExtractor_MainVideo(benchmark::State& state) {
    const fs::path video = fixture_path(kMainVideoRel);
    if (!fs::is_regular_file(video)) {
        state.SkipWithError("Missing main video fixture");
        return;
    }

    BenchTempDir scratch("text_crop");
    const Ffmpeg ff;
    int run_idx = 0;

    for ([[maybe_unused]] auto _ : state) {
        const fs::path out_dir = scratch.path() / std::format("run_{}", run_idx++);
        TextCropExtractor extractor;
        bool ok = extractor.run(ff, video, out_dir);
        benchmark::DoNotOptimize(ok);
        state.PauseTiming();
        std::error_code ec;
        fs::remove_all(out_dir, ec);
        state.ResumeTiming();
    }
}
BENCHMARK(BM_TextCropExtractor_MainVideo)->Unit(benchmark::kMillisecond)->Iterations(2);

}  // namespace

int main(int argc, char** argv) {
    // Suppress verbose info/warning logs during benchmark iterations so
    // console output and --benchmark_out JSON remain clean and unbiased by I/O.
    boost::log::core::get()->set_filter(
        boost::log::trivial::severity >= boost::log::trivial::error);

    bool print_profile = false;
    std::vector<char*> filtered_argv;
    filtered_argv.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--profile-summary") {
            print_profile = true;
        } else {
            filtered_argv.push_back(argv[i]);
        }
    }

    int bench_argc = static_cast<int>(filtered_argv.size());
    benchmark::Initialize(&bench_argc, filtered_argv.data());
    if (benchmark::ReportUnrecognizedArguments(bench_argc, filtered_argv.data())) {
        return 1;
    }

    std::optional<core::ScopedProfilerEnable> profile_guard;
    if (print_profile) {
        core::Profiler::global().reset();
        profile_guard.emplace(core::Profiler::global(), true);
    }

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    if (print_profile) {
        std::cout << "\n=== Profiler Breakdown ===\n"
                  << core::Profiler::global().format_summary();
    }
    return 0;
}
