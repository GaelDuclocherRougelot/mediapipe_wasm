#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "mediapipe/framework/calculator_graph.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/port/status.h"

namespace mediapipe {

absl::Status RunQuantiqDemo() {
  // PassThroughCalculator (framework de base) suivi du ExclaimCalculator
  // (calculator maison) pour verifier que le pipeline custom compile et
  // s'execute correctement.
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
  MP_RETURN_IF_ERROR(graph.Initialize(config));
  MP_ASSIGN_OR_RETURN(OutputStreamPoller poller,
                       graph.AddOutputStreamPoller("out"));
  MP_RETURN_IF_ERROR(graph.StartRun({}));
  for (int i = 0; i < 5; ++i) {
    MP_RETURN_IF_ERROR(graph.AddPacketToInputStream(
        "in", MakePacket<std::string>("Hello Quantiq").At(Timestamp(i))));
  }
  MP_RETURN_IF_ERROR(graph.CloseInputStream("in"));
  mediapipe::Packet packet;
  while (poller.Next(&packet)) {
    ABSL_LOG(INFO) << packet.Get<std::string>();
  }
  return graph.WaitUntilDone();
}
}  // namespace mediapipe

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  ABSL_CHECK(mediapipe::RunQuantiqDemo().ok());
  return 0;
}
