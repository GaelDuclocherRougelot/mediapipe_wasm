load("@rules_cc//cc:cc_library.bzl", "cc_library")

licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "exclaim_calculator",
    srcs = ["exclaim_calculator.cc"],
    alwayslink = 1,
    deps = [
        "//mediapipe/framework:calculator_framework",
    ],
)

cc_library(
    name = "square_overlay_calculator",
    srcs = ["square_overlay_calculator.cc"],
    alwayslink = 1,
    deps = [
        "//mediapipe/framework/port:ret_check",
        "//mediapipe/framework/port:status",
        "//mediapipe/gpu:gl_simple_calculator",
        "//mediapipe/gpu:gl_simple_shaders",
        "//mediapipe/gpu:shader_util",
    ],
)
