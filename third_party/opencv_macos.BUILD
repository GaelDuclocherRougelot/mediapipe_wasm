load("@bazel_skylib//lib:paths.bzl", "paths")

licenses(["notice"])  # BSD license

exports_files(["LICENSE"])

# Symlink Homebrew stable : /opt/homebrew/opt/opencv -> Cellar/opencv/<version courante>
PREFIX = "opt/opencv"

cc_library(
    name = "opencv",
    srcs = glob([
        paths.join(PREFIX, "lib/libopencv_core.dylib"),
        paths.join(PREFIX, "lib/libopencv_calib3d.dylib"),
        paths.join(PREFIX, "lib/libopencv_features2d.dylib"),
        paths.join(PREFIX, "lib/libopencv_highgui.dylib"),
        paths.join(PREFIX, "lib/libopencv_imgcodecs.dylib"),
        paths.join(PREFIX, "lib/libopencv_imgproc.dylib"),
        paths.join(PREFIX, "lib/libopencv_video.dylib"),
        paths.join(PREFIX, "lib/libopencv_videoio.dylib"),
    ]),
    hdrs = glob([paths.join(PREFIX, "include/opencv4/opencv2/**/*.h*")]),
    includes = [paths.join(PREFIX, "include/opencv4/")],
    linkstatic = 1,
    visibility = ["//visibility:public"],
)
