#include <string>

#include "mediapipe/framework/calculator_framework.h"

namespace mediapipe {

// Appends "!!!" to the input string packet and forwards it downstream.
class ExclaimCalculator : public CalculatorBase {
 public:
  static absl::Status GetContract(CalculatorContract* cc) {
    cc->Inputs().Index(0).Set<std::string>();
    cc->Outputs().Index(0).Set<std::string>();
    return absl::OkStatus();
  }

  absl::Status Process(CalculatorContext* cc) override {
    const std::string& input = cc->Inputs().Index(0).Get<std::string>();
    cc->Outputs().Index(0).AddPacket(
        MakePacket<std::string>(input + "!!!").At(cc->InputTimestamp()));
    return absl::OkStatus();
  }
};

REGISTER_CALCULATOR(ExclaimCalculator);

}  // namespace mediapipe
