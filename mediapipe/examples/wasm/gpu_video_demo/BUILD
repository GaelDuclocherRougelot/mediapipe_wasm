load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")

licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "gpu_video_demo",
    srcs = ["gpu_video_demo_wasm.cc"],
    linkopts = select({
        "//mediapipe:emscripten": [
            "-lembind",
            "-sMODULARIZE=1",
            "-sEXPORT_NAME=GpuVideoDemoModule",
            "-sENVIRONMENT=web,worker",
            "-sALLOW_MEMORY_GROWTH=1",
            "-sEXIT_RUNTIME=0",
            "-pthread",
            "-sUSE_PTHREADS=1",
            "-sPTHREAD_POOL_SIZE=4",
        ],
        "//conditions:default": [],
    }),
    deps = [
        "//mediapipe/calculators/quantiq_demo:square_overlay_calculator",
        "//mediapipe/framework:calculator_graph",
        "//mediapipe/framework/formats:image_frame",
        "//mediapipe/framework/port:parse_text_proto",
        "//mediapipe/framework/port:status",
        "//mediapipe/gpu:gl_calculator_helper",
        "//mediapipe/gpu:gpu_buffer",
        "//mediapipe/gpu:gpu_buffer_format",
        "//mediapipe/gpu:gpu_shared_data_internal",
        "@com_google_absl//absl/log:absl_check",
    ],
)

wasm_cc_binary(
    name = "gpu_video_demo_wasm",
    cc_target = ":gpu_video_demo",
    outputs = [
        "gpu_video_demo.js",
        "gpu_video_demo.wasm",
    ],
)
