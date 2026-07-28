#ifndef __EMSCRIPTEN__
#error "This file must only be compiled for the emscripten/wasm platform."
#endif  // __EMSCRIPTEN__

#include <string>
#include <utility>

#include <emscripten/bind.h>

#include "absl/log/absl_check.h"
#include "mediapipe/framework/calculator_graph.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/port/status.h"

namespace mediapipe {
namespace {

// Runs PassThroughCalculator -> ExclaimCalculator on a single input string
// and returns the single output packet's string payload. Blocking,
// single-shot: builds a fresh graph per call.
std::string ProcessString(const std::string& input) {
  CalculatorGraphConfig config =
      ParseTextProtoOrDie<CalculatorGraphConfig>(R"pb(
        input_stream: "in"
        output_stream: "out"
        node {
          calculator: "PassThroughCalculator"
          input_stream: "in"
          output_stream: "out1"
        }
        node {
          calculator: "ExclaimCalculator"
          input_stream: "out1"
          output_stream: "out"
        }
      )pb");

  CalculatorGraph graph;
  ABSL_CHECK_OK(graph.Initialize(config));

  // Note: on Emscripten, CalculatorGraph runs the whole graph synchronously
  // on the calling (application) thread; there is no worker thread that
  // could ever unblock a poller's blocking Next() call, so a blocking
  // OutputStreamPoller would deadlock here. ObserveOutputStream's callback
  // is instead invoked synchronously as part of the scheduler's own pump
  // loop (driven by WaitUntilDone() below), which works with that model.
  std::string result;
  ABSL_CHECK_OK(graph.ObserveOutputStream(
      "out", [&result](const Packet& packet) -> absl::Status {
        result = packet.Get<std::string>();
        return absl::OkStatus();
      }));

  ABSL_CHECK_OK(graph.StartRun({}));
  ABSL_CHECK_OK(graph.AddPacketToInputStream(
      "in", MakePacket<std::string>(input).At(Timestamp(0))));
  ABSL_CHECK_OK(graph.CloseInputStream("in"));
  ABSL_CHECK_OK(graph.WaitUntilDone());
  return result;
}

}  // namespace
}  // namespace mediapipe

EMSCRIPTEN_BINDINGS(quantiq_demo_module) {
  emscripten::function("processString", &mediapipe::ProcessString);
}
