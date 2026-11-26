"""Local Conan recipe: FFmpeg static libraries with NVENC + libx264 + libsvtav1 + libaom.

Why this exists
---------------
Conan-Center's stock `ffmpeg/X.Y` recipe has no NVENC support — its build args
explicitly include `--disable-cuda` / `--disable-cuvid` and there is no
`with_nvcodec` option. The transcode tool in this repo uses `h264_nvenc` and
`av1_nvenc` (plus CUDA/NVDEC hardware decoding), so we maintain our own recipe.

This recipe builds and packages ONLY the pruned static `libav*` libraries and
headers (`--disable-everything`, `--disable-programs`, `--disable-avdevice`,
`--disable-postproc`, `--disable-network`, `--disable-autodetect`) with explicit
format/codec/filter allowlists and per-library Conan components, so each C++
tool links only the FFmpeg libraries it actually needs with deterministic
dependencies across Windows and Linux.

Build prerequisites (handled by Conan automatically via tool_requires):
  * nasm    — assembler for x86/x86_64 SIMD optimisations
  * msys2   — POSIX shell + coreutils so ffmpeg's ./configure runs on Windows
  * pkgconf — isolated via PKG_CONFIG_LIBDIR to locate only Conan dependencies

NVENC details: enabling NVENC/NVDEC at compile time only requires
`nv-codec-headers` (a BSD-licensed shim of NVIDIA's headers). It does **not**
require the CUDA SDK or `nvcc`. At runtime ffmpeg dlopens the NVIDIA driver —
so users without an NVIDIA GPU gracefully fall back to software codecs.

Licence: libx264 is GPLv2+, so linking libav* statically makes every consuming
executable GPL. This project is GPLv3 (see LICENSE at the repo root), so that
is already the case and no additional obligation is created.
"""

import os
import shutil

from conan import ConanFile
from conan.tools.env import VirtualBuildEnv
from conan.tools.files import copy, get, rmdir
from conan.tools.gnu import Autotools, AutotoolsToolchain, PkgConfigDeps
from conan.tools.layout import basic_layout
from conan.tools.microsoft import is_msvc


class FFmpegNvencConan(ConanFile):
    name = "ffmpeg-nvenc"
    version = "7.1.4"
    description = "FFmpeg static libraries built from source with NVENC + libx264 + libsvtav1 + libaom."
    license = "GPL-2.0-or-later"  # GPL because of libx264.
    url = "https://github.com/MasakatsuFunaki/ffmpeg-concat"
    homepage = "https://ffmpeg.org"
    settings = "os", "arch", "compiler", "build_type"
    package_type = "static-library"

    # ------------------------------------------------------------------ #
    # Settings hygiene
    # ------------------------------------------------------------------ #
    def configure(self):
        # libav* is C, so the C++ settings of a consumer cannot change what gets
        # built here; strip them so the package ID doesn't churn.
        self.settings.rm_safe("compiler.cppstd")
        self.settings.rm_safe("compiler.libcxx")

    # ------------------------------------------------------------------ #
    # Dependency graph
    # ------------------------------------------------------------------ #
    def requirements(self):
        # NVENC/NVDEC build-time interface (BSD shim, no CUDA SDK needed).
        self.requires("nv-codec-headers/13.0.19.0")
        # Encoders.
        self.requires("libx264/cci.20250910")
        self.requires("libsvtav1/2.2.1")
        self.requires("libaom-av1/3.13.3")

    def build_requirements(self):
        if self.settings.arch in ("x86", "x86_64"):
            self.tool_requires("nasm/2.16.01")
        self.tool_requires("pkgconf/[>=2.1 <3]")
        if self._settings_build.os == "Windows":
            self.win_bash = True
            if not self.conf.get("tools.microsoft.bash:path", check_type=str):
                self.tool_requires("msys2/cci.latest")

    @property
    def _settings_build(self):
        # settings_build is only populated when cross-building.
        return getattr(self, "settings_build", self.settings)

    # ------------------------------------------------------------------ #
    # Source & layout
    # ------------------------------------------------------------------ #
    def layout(self):
        basic_layout(self, src_folder="src")

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)

    # ------------------------------------------------------------------ #
    # Generate (toolchain + deps for ./configure)
    # ------------------------------------------------------------------ #
    def generate(self):
        VirtualBuildEnv(self).generate()

        tc = AutotoolsToolchain(self)
        tc.configure_args = [
            "--prefix=/",
            # Prevent host libraries (zlib, bzlib, lzma, alsa, xcb, vaapi,
            # vdpau, sdl2, etc.) from silently linking into static libav*.
            "--disable-autodetect",
            "--disable-programs",
            "--disable-avdevice",
            "--disable-postproc",
            "--disable-network",
            "--disable-everything",
            "--enable-w32threads" if self.settings.os == "Windows" else "--enable-pthreads",
            "--disable-doc",
            "--disable-debug",
            "--disable-htmlpages",
            "--disable-manpages",
            "--disable-podpages",
            "--disable-txtpages",
            "--disable-shared",
            "--enable-static",
            # GPL is required by libx264 and boxblur.
            "--enable-gpl",
            # External encoders/decoders we actually use.
            "--enable-libx264",
            "--enable-libsvtav1",
            "--enable-libaom",
            # NVIDIA hwaccels/encoders: header-only at build time; resolve the
            # driver at runtime (--disable-autodetect requires explicit flags).
            "--enable-ffnvcodec",
            "--enable-cuda",
            "--enable-nvdec",
            "--enable-nvenc",
            "--enable-cuvid",
            # Conan's pkgconf package installs the binary as `pkgconf(.exe)`,
            # whereas ffmpeg's configure looks for `pkg-config` by default.
            "--pkg-config=pkgconf",
            "--pkg-config-flags=--static",
        ]

        # Explicit component allowlists covering all five tools and common
        # real-world video/audio/image formats (.mp4, .mov, .m4v, .mkv, .webm,
        # .avi, .flv, .wmv, .ts, .jpg, .png, .bmp).
        demuxers = (
            "mov", "matroska", "avi", "flv", "mpegts", "asf", "mpegps",
            "ogg", "wav", "mp3", "aac", "flac",
            "image2", "mjpeg", "rawvideo", "h264", "hevc", "ivf",
        )
        muxers = (
            "mp4", "mov", "matroska", "webm", "avi", "flv", "mpegts", "asf",
            "image2", "mjpeg", "null",
        )
        decoders = (
            # Video
            "h264", "hevc", "av1", "libaom_av1", "vp8", "vp9",
            "mpeg4", "mpeg2video", "mpeg1video", "msmpeg4v3",
            "wmv1", "wmv2", "wmv3", "vc1", "flv", "h263",
            "mjpeg", "png", "bmp", "rawvideo", "prores", "theora",
            # NVIDIA CUVID
            "h264_cuvid", "hevc_cuvid", "av1_cuvid", "vp8_cuvid", "vp9_cuvid",
            # Audio
            "aac", "aac_latm", "mp3", "mp2", "opus", "vorbis",
            "ac3", "eac3", "flac", "alac", "wmav1", "wmav2",
            "pcm_s16le", "pcm_s16be", "pcm_s24le", "pcm_s24be",
            "pcm_s32le", "pcm_f32le", "pcm_u8", "pcm_mulaw", "pcm_alaw",
        )
        encoders = (
            # Video (configure uses underscore `libaom_av1`; runtime name is `libaom-av1`)
            "libx264", "libsvtav1", "libaom_av1",
            "h264_nvenc", "av1_nvenc", "hevc_nvenc",
            "mjpeg", "png", "bmp",
            # Audio
            "aac", "pcm_s16le",
        )
        hwaccels = (
            "h264_nvdec", "hevc_nvdec", "av1_nvdec", "vp8_nvdec", "vp9_nvdec",
            "mpeg1_nvdec", "mpeg2_nvdec", "mpeg4_nvdec", "vc1_nvdec", "mjpeg_nvdec",
        )
        parsers = (
            "h264", "hevc", "av1", "vp8", "vp9",
            "mpeg4video", "mpegvideo", "mjpeg", "vc1",
            "aac", "aac_latm", "mpegaudio", "opus", "vorbis", "ac3", "flac",
        )
        bsfs = (
            "h264_mp4toannexb", "hevc_mp4toannexb", "aac_adtstoasc",
            "extract_extradata",
            "av1_frame_split", "av1_frame_merge", "av1_metadata",
            "vp9_superframe", "vp9_superframe_split", "null",
        )
        protocols = ("file", "pipe")
        # Note: `fifo`/`afifo` were removed in FFmpeg 7.0+ (graph scheduler buffers natively).
        filters = (
            "buffer", "abuffer", "buffersink", "abuffersink",
            "null", "anull", "nullsink", "anullsink", "copy",
            "fps", "concat", "split", "gblur", "boxblur",
            "scale", "overlay", "volume",
            "format", "aformat", "aresample",
            "setpts", "asetpts", "trim", "atrim",
        )

        for kind, names in (
            ("demuxer", demuxers),
            ("muxer", muxers),
            ("decoder", decoders),
            ("encoder", encoders),
            ("hwaccel", hwaccels),
            ("parser", parsers),
            ("bsf", bsfs),
            ("protocol", protocols),
            ("filter", filters),
        ):
            tc.configure_args.extend(f"--enable-{kind}={name}" for name in names)

        if self.settings.os != "Windows":
            # Compile C sources with -fPIC and x86_64 NASM objects with -DPIC
            # so static libav* links cleanly into PIE executables on Linux.
            tc.configure_args.append("--enable-pic")
        if is_msvc(self):
            tc.configure_args.append("--toolchain=msvc")

        # Isolate pkgconf from host /usr/lib/*/pkgconfig directories.
        env = tc.environment()
        env.define_path("PKG_CONFIG_LIBDIR", self.generators_folder)
        tc.generate(env)

        PkgConfigDeps(self).generate()

    # ------------------------------------------------------------------ #
    # Build
    # ------------------------------------------------------------------ #
    def build(self):
        gen = self.generators_folder

        # ffmpeg's configure looks for `x264.pc` on disk; CCI's libx264 recipe
        # ships `libx264.pc`. Mirror it under the legacy name so configure's
        # pkg-config probe finds it on every platform.
        for src_name, dst_name in (("libx264.pc", "x264.pc"), ("x264.pc", "libx264.pc")):
            src = os.path.join(gen, src_name)
            dst = os.path.join(gen, dst_name)
            if os.path.exists(src) and not os.path.exists(dst):
                shutil.copy(src, dst)

        # MSVC lib-naming shim.
        # ffmpeg's configure (--toolchain=msvc) translates `-lfoo` from each
        # dependency's .pc file into the literal link argument `libfoo.lib`.
        # But Conan's MSVC packages name their libraries WITHOUT the `lib`
        # prefix (`x264.lib`, `aom.lib`, `SvtAv1Enc.lib`, `cpuinfo.lib`).
        # Symptom: `LNK1181: cannot open input file 'libx264.lib'`.
        # Fix: copy each dep's .lib file into a shim directory under its
        # `lib*.lib` alias, and prepend `-L<shim_dir>` to every .pc Libs line
        # so pkgconf includes a `-libpath:<shim_dir>` ahead of the real libdir.
        if is_msvc(self):
            shim_dir = os.path.join(self.build_folder, "msvc_lib_shims")
            os.makedirs(shim_dir, exist_ok=True)

            # libname-without-prefix -> conan dep name
            shim_targets = {
                "x264": "libx264",
                "aom": "libaom-av1",
                "SvtAv1Enc": "libsvtav1",
                "cpuinfo": "cpuinfo",
            }
            for libname, dep_name in shim_targets.items():
                if dep_name not in self.dependencies:
                    continue
                dep = self.dependencies[dep_name]
                libdirs = dep.cpp_info.libdirs or []
                for libdir in libdirs:
                    src_lib = os.path.join(libdir, f"{libname}.lib")
                    if os.path.exists(src_lib):
                        shutil.copy(src_lib, os.path.join(shim_dir, f"lib{libname}.lib"))
                        break

            shim_dir_unix = shim_dir.replace("\\", "/")
            for pc_name in os.listdir(gen):
                if not pc_name.endswith(".pc"):
                    continue
                pc_path = os.path.join(gen, pc_name)
                with open(pc_path, "r", encoding="utf-8") as f:
                    text = f.read()
                new_text = text.replace(
                    "Libs: -L",
                    f'Libs: -L"{shim_dir_unix}" -L',
                    1,  # only the first occurrence per file
                )
                if new_text != text:
                    with open(pc_path, "w", encoding="utf-8") as f:
                        f.write(new_text)

        autotools = Autotools(self)
        autotools.configure()
        autotools.make()

    # ------------------------------------------------------------------ #
    # Package: static libav* libraries + headers.
    # ------------------------------------------------------------------ #
    def package(self):
        # Always install LICENSE-equivalent files.
        copy(self, "COPYING*", self.source_folder, os.path.join(self.package_folder, "licenses"))
        copy(self, "LICENSE*", self.source_folder, os.path.join(self.package_folder, "licenses"))

        Autotools(self).install()
        self._flatten_msys_install()

        # pkg-config metadata and share/ examples are redundant — consumers use CMakeDeps.
        rmdir(self, os.path.join(self.package_folder, "lib", "pkgconfig"))
        rmdir(self, os.path.join(self.package_folder, "share"))

    def _flatten_msys_install(self):
        """Lift bin/, lib/ and include/ out of any nested MSYS install tree.

        Fallback for Windows MSYS2 path translation where $(DESTDIR)$(prefix)
        can yield a nested tree under <pkg>/<msys-prefix>/lib/avcodec.lib.
        """
        if os.path.isdir(os.path.join(self.package_folder, "include", "libavcodec")):
            return  # installed directly under package_folder

        for root, _, _ in os.walk(self.package_folder):
            if not os.path.isdir(os.path.join(root, "include", "libavcodec")):
                continue
            for sub in ("bin", "lib", "include"):
                src = os.path.join(root, sub)
                if not os.path.isdir(src):
                    continue
                dst = os.path.join(self.package_folder, sub)
                os.makedirs(dst, exist_ok=True)
                for entry in os.listdir(src):
                    shutil.move(os.path.join(src, entry), os.path.join(dst, entry))
            break

        # Drop the now-empty nested tree (anything that is not a real output).
        for entry in os.listdir(self.package_folder):
            if entry in ("bin", "lib", "include", "licenses"):
                continue
            path = os.path.join(self.package_folder, entry)
            if os.path.isdir(path):
                rmdir(self, path)
            elif os.path.isfile(path):
                os.remove(path)

    # ------------------------------------------------------------------ #
    # Consumer-facing info (per-library Conan 2 components)
    # ------------------------------------------------------------------ #
    def package_info(self):
        # Expose each static FFmpeg library as its own Conan component so
        # lightweight tools link only what they use (e.g. ffmpeg-nvenc::avformat
        # without avfilter/swscale/swresample). CMakeDeps also generates the
        # full aggregate target ffmpeg-nvenc::ffmpeg-nvenc automatically.
        def _add_component(name, requires):
            comp = self.cpp_info.components[name]
            comp.libs = [name]
            comp.includedirs = ["include"]
            comp.libdirs = ["lib"]
            comp.bindirs = []
            comp.requires = requires
            return comp

        avutil = _add_component("avutil", ["nv-codec-headers::nv-codec-headers"])
        _add_component("swscale", ["avutil"])
        _add_component("swresample", ["avutil"])
        avcodec = _add_component(
            "avcodec",
            [
                "swresample",
                "avutil",
                "libx264::libx264",
                "libsvtav1::libsvtav1",
                "libaom-av1::libaom-av1",
                "nv-codec-headers::nv-codec-headers",
            ],
        )
        avformat = _add_component("avformat", ["avcodec", "avutil"])
        _add_component(
            "avfilter",
            ["avformat", "avcodec", "swscale", "swresample", "avutil"],
        )

        if self.settings.os == "Windows":
            avutil.system_libs = [
                "bcrypt", "user32", "advapi32", "ole32", "oleaut32",
                "uuid", "shlwapi", "psapi",
            ]
            avcodec.system_libs = [
                "mfplat", "mfuuid", "strmiids", "ole32", "user32",
                "gdi32", "vfw32",
            ]
            avformat.system_libs = [
                "ws2_32", "secur32", "bcrypt", "shlwapi",
            ]
        else:
            avutil.system_libs = ["m", "pthread", "dl"]
            if self.settings.os == "Linux":
                avutil.exelinkflags = ["-Wl,-Bsymbolic"]
                avutil.sharedlinkflags = ["-Wl,-Bsymbolic"]
