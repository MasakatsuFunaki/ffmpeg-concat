from conan import ConanFile
from conan.tools.cmake import cmake_layout


class FFmpegToolsConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("boost/1.84.0")
        self.requires("gtest/1.14.0")
        self.requires("opencv/4.9.0")
        self.requires("openssl/3.2.1")
        self.requires("indicators/2.3")
        # FFmpeg static libav* libraries built from source with NVENC + libx264 +
        # libsvtav1 + libaom. Recipe lives in ./external/ffmpeg/ — register with
        # `conan export external/ffmpeg` before `conan install . --build=missing`.
        self.requires("ffmpeg-nvenc/7.1.4")

    def configure(self):
        # Trim OpenCV to ONLY the modules used by extract (core, imgproc,
        # imgcodecs, features2d, flann). Disabling highgui, videoio, and
        # objdetect automatically drops their GUI/video/QR options (with_wayland,
        # with_gtk, with_ffmpeg, with_quirc) and transitive system dependencies.
        opencv = self.options["opencv"]
        opencv.calib3d = False
        opencv.dnn = False
        opencv.gapi = False
        opencv.highgui = False
        opencv.ml = False
        opencv.objdetect = False
        opencv.photo = False
        opencv.stitching = False
        opencv.video = False
        opencv.videoio = False
        opencv.with_eigen = False
        opencv.with_openexr = False
        opencv.with_tiff = False
        opencv.with_webp = False
        opencv.with_jpeg2000 = False

    def layout(self):
        cmake_layout(self)
