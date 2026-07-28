load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")

licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "quantiq_demo",
    srcs = ["quantiq_demo_wasm.cc"],
    linkopts = select({
        "//mediapipe:emscripten": [
            "-lembind",
            "-sMODULARIZE=1",
            "-sEXPORT_NAME=QuantiqModule",
            "-sENVIRONMENT=web",
            "-sALLOW_MEMORY_GROWTH=1",
            "-sEXIT_RUNTIME=0",
        ],
        "//conditions:default": [],
    }),
    deps = [
        "//mediapipe/calculators/core:pass_through_calculator",
        "//mediapipe/calculators/quantiq_demo:exclaim_calculator",
        "//mediapipe/framework:calculator_graph",
        "//mediapipe/framework/port:parse_text_proto",
        "//mediapipe/framework/port:status",
        "@com_google_absl//absl/log:absl_check",
    ],
)

wasm_cc_binary(
    name = "quantiq_demo_wasm",
    cc_target = ":quantiq_demo",
    outputs = [
        "quantiq_demo.js",
        "quantiq_demo.wasm",
    ],
)
