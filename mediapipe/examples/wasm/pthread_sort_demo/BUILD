load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")

licenses(["notice"])

package(default_visibility = ["//visibility:public"])

# Standalone proof of concept: no MediaPipe framework dependency at all, just
# the C++ standard library + Embind. Confirms real parallel std::thread work
# under Emscripten -pthread (see README.md section 3.8/3.9).
cc_binary(
    name = "pthread_sort_demo",
    srcs = ["pthread_sort_demo_wasm.cc"],
    copts = select({
        "//mediapipe:emscripten": ["-pthread"],
        "//conditions:default": [],
    }),
    linkopts = select({
        "//mediapipe:emscripten": [
            "-lembind",
            "-sMODULARIZE=1",
            "-sEXPORT_NAME=PthreadSortDemoModule",
            "-sENVIRONMENT=web,worker",
            "-sALLOW_MEMORY_GROWTH=1",
            "-sEXIT_RUNTIME=0",
            "-pthread",
            "-sUSE_PTHREADS=1",
            "-sPTHREAD_POOL_SIZE=8",
        ],
        "//conditions:default": [],
    }),
)

wasm_cc_binary(
    name = "pthread_sort_demo_wasm",
    cc_target = ":pthread_sort_demo",
    outputs = [
        "pthread_sort_demo.js",
        "pthread_sort_demo.wasm",
    ],
)
