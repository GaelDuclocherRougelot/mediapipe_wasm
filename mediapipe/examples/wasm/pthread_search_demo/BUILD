load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")

licenses(["notice"])

package(default_visibility = ["//visibility:public"])

# Standalone proof of concept: no MediaPipe framework dependency at all, just
# the C++ standard library + Embind. Parallel search with atomic-based early
# stop and no merge phase (contrast with pthread_sort_demo/, which needs one).
cc_binary(
    name = "pthread_search_demo",
    srcs = ["pthread_search_demo_wasm.cc"],
    copts = select({
        "//mediapipe:emscripten": ["-pthread"],
        "//conditions:default": [],
    }),
    linkopts = select({
        "//mediapipe:emscripten": [
            "-lembind",
            "-sMODULARIZE=1",
            "-sEXPORT_NAME=PthreadSearchDemoModule",
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
    name = "pthread_search_demo_wasm",
    cc_target = ":pthread_search_demo",
    outputs = [
        "pthread_search_demo.js",
        "pthread_search_demo.wasm",
    ],
)
