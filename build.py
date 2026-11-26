#!/usr/bin/env python3
"""Unified build, test, clean, install, and release driver for ffmpeg-tools."""

import argparse
import os
import platform
import shutil
import subprocess
import sys

PROJECT_ROOT = os.path.dirname(os.path.realpath(__file__))

IS_WINDOWS = platform.system() == "Windows"
DEFAULT_PRESET = "conan-windows-release" if IS_WINDOWS else "conan-linux-release"

# Conan writes CMakeUserPresets.json at the repo root and accumulates one
# include per build layout it has ever generated (e.g. Windows 'build/generators'
# and Linux 'build/Release/generators'). Deleting it before each Conan install
# ensures CMakeUserPresets.json only references the active host layout.
CONAN_USER_PRESETS = os.path.join(PROJECT_ROOT, "CMakeUserPresets.json")
BUILD_DIR = os.path.join(PROJECT_ROOT, "build")


def run_cmd(cmd):
    print(f"Running: {' '.join(cmd)}")
    subprocess.run(cmd, check=True, cwd=PROJECT_ROOT)


def rm(path):
    """Remove a file or directory tree if present (best effort)."""
    if os.path.isdir(path):
        print(f"Removing {path}")
        shutil.rmtree(path, ignore_errors=True)
    elif os.path.exists(path):
        print(f"Removing {path}")
        os.remove(path)


def linux_sys_pkg_conf():
    """Return Conan system package-manager flags on Linux, guarding sudo cleanly."""
    if IS_WINDOWS:
        return []
    conf = ["-c", "tools.system.package_manager:mode=install"]
    if hasattr(os, "geteuid") and os.geteuid() != 0 and shutil.which("sudo") is not None:
        conf.extend(["-c", "tools.system.package_manager:sudo=True"])
    return conf


def split_passthrough(argv):
    """Split arguments before and after '--' so extra args forward to ctest or benchmarks."""
    if "--" in argv:
        sep = argv.index("--")
        return argv[:sep], argv[sep + 1:]
    return argv, []


def find_benchmark_executable(preset):
    """Locate the built ffmpeg_tools_bench binary across single- and multi-config generators."""
    binary_dir = "build/Release" if "linux" in preset else "build"
    exe_name = "ffmpeg_tools_bench.exe" if IS_WINDOWS else "ffmpeg_tools_bench"
    candidates = [
        os.path.join(PROJECT_ROOT, binary_dir, "benchmark", "bin", "Release", exe_name),
        os.path.join(PROJECT_ROOT, binary_dir, "benchmark", "bin", exe_name),
        os.path.join(PROJECT_ROOT, binary_dir, "benchmark", "Release", exe_name),
        os.path.join(PROJECT_ROOT, binary_dir, "benchmark", exe_name),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    raise FileNotFoundError(
        f"Benchmark executable '{exe_name}' not found. Checked: {', '.join(candidates)}"
    )


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Build, test, package, and release the ffmpeg-tools suite via Conan 2 + CMake presets. "
            "Extra arguments after '--' are forwarded directly to ctest."
        )
    )
    parser.add_argument(
        "--profile",
        default="default",
        help="Conan profile to use (default: default)",
    )
    parser.add_argument(
        "--preset",
        default=DEFAULT_PRESET,
        help=f"CMake preset to build (default on this host: {DEFAULT_PRESET})",
    )
    parser.add_argument(
        "--jobs", "-j",
        type=int,
        default=os.cpu_count() or 1,
        help="Parallel compile jobs (default: all CPU cores)",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Remove build/ and CMakeUserPresets.json before building",
    )
    parser.add_argument(
        "--purge-cache",
        action="store_true",
        help="Also purge cached 'ffmpeg-nvenc/*' packages from the local Conan cache",
    )
    parser.add_argument(
        "--install",
        action="store_true",
        help="Run 'cmake --install' after building (populates install/<os>/; strips on Linux)",
    )
    parser.add_argument(
        "--test",
        action="store_true",
        help="Configure with -DBUILD_TESTING=ON, build, and run ctest",
    )
    parser.add_argument(
        "--tier",
        type=str.upper,
        choices=["UT", "CT", "ST"],
        default=None,
        help="Run only the selected test tier (UT, CT, or ST via 'ctest -L <tier>'); implies --test",
    )
    parser.add_argument(
        "--benchmark",
        action="store_true",
        help="Configure with -DBUILD_BENCHMARKS=ON, build, and run the Google Benchmark suite",
    )
    parser.add_argument(
        "--deploy",
        action="store_true",
        help=(
            "Tag 'main' and publish a GitHub Release via release.py "
            "(triggers GitHub Actions CI & Release by default; combine with --local to upload locally)"
        ),
    )
    parser.add_argument(
        "--local",
        action="store_true",
        help="When used with --deploy, forward --local to release.py to build, package, and upload locally",
    )

    our_argv, passthrough_extra = split_passthrough(sys.argv[1:])
    args = parser.parse_args(our_argv)

    run_tests = args.test or (args.tier is not None) or (bool(passthrough_extra) and not args.benchmark)
    ctest_extra = passthrough_extra if run_tests else []
    bench_extra = passthrough_extra if (args.benchmark and not run_tests) else []

    if args.clean:
        print("\n--- Cleaning build tree ---")
        rm(BUILD_DIR)
    rm(CONAN_USER_PRESETS)

    if args.purge_cache:
        print("\n--- Purging local ffmpeg-nvenc Conan cache ---")
        run_cmd(["conan", "remove", "ffmpeg-nvenc/*", "-c"])

    # If --deploy is requested without an explicit local test/install/benchmark step, hand
    # off directly to release.py (which tags 'main' for GitHub Actions, or runs
    # 'build.py --install' itself when --local is passed).
    if args.deploy and not run_tests and not args.install and not args.benchmark:
        release_cmd = [sys.executable, os.path.join(PROJECT_ROOT, "release.py")]
        if args.local:
            release_cmd.append("--local")
        print("\n--- Running release pipeline (release.py) ---")
        run_cmd(release_cmd)
        return

    sys_pkg_conf = linux_sys_pkg_conf()

    # Export the local ffmpeg-nvenc recipe metadata cleanly without forcing a
    # rebuild; 'conan install --build=missing' reuses cached binaries instantly.
    print("\n--- Exporting local ffmpeg-nvenc recipe ---")
    run_cmd(["conan", "export", "external/ffmpeg"])

    print("\n--- Installing Conan dependencies ---")
    run_cmd(
        ["conan", "install", ".", "--build=missing", f"-pr={args.profile}"]
        + sys_pkg_conf
    )

    print("\n--- Configuring CMake ---")
    configure_cmd = [
        "cmake",
        "--preset",
        args.preset,
        f"-DBUILD_TESTING={'ON' if run_tests else 'OFF'}",
        f"-DBUILD_BENCHMARKS={'ON' if args.benchmark else 'OFF'}",
    ]
    run_cmd(configure_cmd)

    print(f"\n--- Building (parallel jobs: {args.jobs}) ---")
    run_cmd(
        ["cmake", "--build", "--preset", args.preset, "--parallel", str(args.jobs)]
    )

    if args.install:
        binary_dir = "build/Release" if "linux" in args.preset else "build"
        install_cmd = ["cmake", "--install", binary_dir, "--config", "Release"]
        if not IS_WINDOWS:
            install_cmd.append("--strip")
        print("\n--- Installing deliverables ---")
        run_cmd(install_cmd)

    if run_tests:
        ctest_cmd = ["ctest", "--preset", args.preset, "--output-on-failure"]
        if args.tier is not None:
            ctest_cmd.extend(["-L", args.tier])
        if ctest_extra:
            ctest_cmd.extend(ctest_extra)
        print("\n--- Running Tests ---")
        run_cmd(ctest_cmd)

    if args.benchmark:
        bench_exe = find_benchmark_executable(args.preset)
        bench_cmd = [bench_exe] + bench_extra
        print("\n--- Running Benchmarks ---")
        run_cmd(bench_cmd)

    if args.deploy:
        release_cmd = [sys.executable, os.path.join(PROJECT_ROOT, "release.py")]
        if args.local:
            release_cmd.append("--local")
        print("\n--- Running release pipeline (release.py) ---")
        run_cmd(release_cmd)


if __name__ == "__main__":
    main()
