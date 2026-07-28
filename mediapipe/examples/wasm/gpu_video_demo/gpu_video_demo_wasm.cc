#ifndef __EMSCRIPTEN__
#error "This file must only be compiled for the emscripten/wasm platform."
#endif  // __EMSCRIPTEN__

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "absl/log/absl_check.h"
#include "mediapipe/framework/calculator_graph.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/packet.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/gpu/gl_calculator_helper.h"
#include "mediapipe/gpu/gpu_buffer.h"
#include "mediapipe/gpu/gpu_buffer_format.h"
#include "mediapipe/gpu/gpu_shared_data_internal.h"

namespace mediapipe {
namespace {

constexpr char kInputStream[] = "input_video";
constexpr char kOutputStream[] = "output_video";

}  // namespace

// Runs a single-node GPU graph (SquareOverlayCalculator) fed frame-by-frame
// from JS. The graph is built once and kept running (StartRun called once, never
// closed); each ProcessFrame call pushes one frame and drains it
// synchronously via WaitUntilIdle() before returning the result.
//
// Note: on Emscripten, CalculatorGraph runs synchronously on the calling
// (application) thread (see Stage 1: calculator_graph.cc forces
// use_application_thread under __EMSCRIPTEN__). A blocking
// OutputStreamPoller would deadlock here, so output is read via
// ObserveOutputStream's callback, invoked synchronously by the scheduler
// pump loop that WaitUntilIdle() drives.
class GpuVideoDemo {
 public:
  // Must be called once, after Module.canvas has been assigned on the JS
  // side (gl_context_webgl.cc hardcodes the WebGL context target to the
  // "#canvas" special HTML target, mapped to Module.canvas).
  std::string Initialize() {
    CalculatorGraphConfig config =
        ParseTextProtoOrDie<CalculatorGraphConfig>(R"pb(
          input_stream: "input_video"
          output_stream: "output_video"
          node {
            calculator: "SquareOverlayCalculator"
            input_stream: "input_video"
            output_stream: "output_video"
          }
        )pb");
    ABSL_CHECK_OK(graph_.Initialize(config));

    auto gpu_resources = mediapipe::GpuResources::Create();
    ABSL_CHECK_OK(gpu_resources.status());
    ABSL_CHECK_OK(graph_.SetGpuResources(std::move(gpu_resources).value()));
    gpu_helper_.InitializeForTest(graph_.GetGpuResources().get());

    ABSL_CHECK_OK(graph_.ObserveOutputStream(
        kOutputStream, [this](const Packet& packet) -> absl::Status {
          return ReadBackFrame(packet.Get<GpuBuffer>());
        }));

    ABSL_CHECK_OK(graph_.StartRun({}));
    initialized_ = true;
    return "ok";
  }

  // Processes one RGBA frame (JS Uint8Array/Uint8ClampedArray of length
  // width*height*4, no row padding). Returns a memory view (NOT a copy) into
  // the wasm heap holding the RGBA luminance result; the caller must copy it
  // out synchronously before yielding to the event loop (see Stage 2 plan,
  // risk: ALLOW_MEMORY_GROWTH may invalidate the view on the next
  // allocation).
  emscripten::val ProcessFrame(emscripten::val rgba, int width, int height) {
    ABSL_CHECK(initialized_);
    std::vector<uint8_t> pixels = emscripten::vecFromJSArray<uint8_t>(rgba);
    ABSL_CHECK_EQ(pixels.size(), static_cast<size_t>(width) * height * 4);

    auto input_frame = std::make_unique<ImageFrame>(
        ImageFormat::SRGBA, width, height, ImageFrame::kGlDefaultAlignmentBoundary);
    input_frame->CopyPixelData(ImageFormat::SRGBA, width, height, pixels.data(),
                               ImageFrame::kGlDefaultAlignmentBoundary);

    size_t timestamp_us = frame_count_++;
    ABSL_CHECK_OK(gpu_helper_.RunInGlContext(
        [this, &input_frame, timestamp_us]() -> absl::Status {
          auto texture = gpu_helper_.CreateSourceTexture(*input_frame);
          auto gpu_frame = texture.GetFrame<GpuBuffer>();
          glFlush();
          texture.Release();
          return graph_.AddPacketToInputStream(
              kInputStream, Adopt(gpu_frame.release()).At(Timestamp(timestamp_us)));
        }));

    ABSL_CHECK_OK(graph_.WaitUntilIdle());

    return emscripten::val(
        emscripten::typed_memory_view(output_buffer_.size(), output_buffer_.data()));
  }

 private:
  absl::Status ReadBackFrame(const GpuBuffer& gpu_frame) {
    return gpu_helper_.RunInGlContext([this, &gpu_frame]() -> absl::Status {
      auto texture = gpu_helper_.CreateSourceTexture(gpu_frame);
      output_buffer_.resize(static_cast<size_t>(texture.width()) * texture.height() * 4);
      gpu_helper_.BindFramebuffer(texture);
      const auto& info = GlTextureInfoForGpuBufferFormat(gpu_frame.format(), 0,
                                                         gpu_helper_.GetGlVersion());
      glReadPixels(0, 0, texture.width(), texture.height(), info.gl_format, info.gl_type,
                  output_buffer_.data());
      glFlush();
      texture.Release();
      return absl::OkStatus();
    });
  }

  CalculatorGraph graph_;
  GlCalculatorHelper gpu_helper_;
  bool initialized_ = false;
  size_t frame_count_ = 0;
  std::vector<uint8_t> output_buffer_;
};

}  // namespace mediapipe

EMSCRIPTEN_BINDINGS(gpu_video_demo_module) {
  emscripten::class_<mediapipe::GpuVideoDemo>("GpuVideoDemo")
      .constructor<>()
      .function("initialize", &mediapipe::GpuVideoDemo::Initialize)
      .function("processFrame", &mediapipe::GpuVideoDemo::ProcessFrame);
}
