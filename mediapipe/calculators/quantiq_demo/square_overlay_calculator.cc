#include "mediapipe/framework/port/ret_check.h"
#include "mediapipe/framework/port/status.h"
#include "mediapipe/gpu/gl_simple_calculator.h"
#include "mediapipe/gpu/gl_simple_shaders.h"
#include "mediapipe/gpu/shader_util.h"

enum { ATTRIB_VERTEX, ATTRIB_TEXTURE_POSITION, NUM_ATTRIBUTES };

namespace mediapipe {

// Passes the input video frame through unchanged, except for a solid-color
// square drawn in the center of the frame. Same GPU pipeline shape as
// LuminanceCalculator (GlSimpleCalculator subclass, single input/output
// stream), just a different fragment shader.
class SquareOverlayCalculator : public GlSimpleCalculator {
 public:
  absl::Status GlSetup() override;
  absl::Status GlRender(const GlTexture& src, const GlTexture& dst) override;
  absl::Status GlTeardown() override;

 private:
  GLuint program_ = 0;
  GLint frame_;
};
REGISTER_CALCULATOR(SquareOverlayCalculator);

absl::Status SquareOverlayCalculator::GlSetup() {
  const GLint attr_location[NUM_ATTRIBUTES] = {
      ATTRIB_VERTEX,
      ATTRIB_TEXTURE_POSITION,
  };
  const GLchar* attr_name[NUM_ATTRIBUTES] = {
      "position",
      "texture_coordinate",
  };

  const GLchar* frag_src = GLES_VERSION_COMPAT
      R"(
#if __VERSION__ < 130
  #define in varying
#endif  // __VERSION__ < 130

#ifdef GL_ES
  #define fragColor gl_FragColor
  precision highp float;
#else
  #define lowp
  #define mediump
  #define highp
  #define texture2D texture
  out vec4 fragColor;
#endif  // defined(GL_ES)

  in vec2 sample_coordinate;
  uniform sampler2D video_frame;

  // Half-size (in normalized [0,1] texture coordinates) of the square drawn
  // at the center of the frame -- 0.15 means the square spans 30% of the
  // width and 30% of the height.
  const highp float kSquareHalfSize = 0.15;
  const highp vec4 kSquareColor = vec4(1.0, 0.0, 0.0, 1.0);

  void main() {
    vec4 color = texture2D(video_frame, sample_coordinate);
    highp vec2 d = abs(sample_coordinate - vec2(0.5, 0.5));
    if (d.x < kSquareHalfSize && d.y < kSquareHalfSize) {
      fragColor = kSquareColor;
    } else {
      fragColor = color;
    }
  }

  )";

  GlhCreateProgram(kBasicVertexShader, frag_src, NUM_ATTRIBUTES,
                   (const GLchar**)&attr_name[0], attr_location, &program_);
  RET_CHECK(program_) << "Problem initializing the program.";
  frame_ = glGetUniformLocation(program_, "video_frame");
  return absl::OkStatus();
}

absl::Status SquareOverlayCalculator::GlRender(const GlTexture& src,
                                               const GlTexture& dst) {
  static const GLfloat square_vertices[] = {
      -1.0f, -1.0f,  // bottom left
      1.0f,  -1.0f,  // bottom right
      -1.0f, 1.0f,   // top left
      1.0f,  1.0f,   // top right
  };
  static const GLfloat texture_vertices[] = {
      0.0f, 0.0f,  // bottom left
      1.0f, 0.0f,  // bottom right
      0.0f, 1.0f,  // top left
      1.0f, 1.0f,  // top right
  };

  glUseProgram(program_);
  glUniform1i(frame_, 1);

  GLuint vbo[2];
  glGenBuffers(2, vbo);
  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);

  glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
  glBufferData(GL_ARRAY_BUFFER, 4 * 2 * sizeof(GLfloat), square_vertices,
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(ATTRIB_VERTEX);
  glVertexAttribPointer(ATTRIB_VERTEX, 2, GL_FLOAT, 0, 0, nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
  glBufferData(GL_ARRAY_BUFFER, 4 * 2 * sizeof(GLfloat), texture_vertices,
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(ATTRIB_TEXTURE_POSITION);
  glVertexAttribPointer(ATTRIB_TEXTURE_POSITION, 2, GL_FLOAT, 0, 0, nullptr);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  glDisableVertexAttribArray(ATTRIB_VERTEX);
  glDisableVertexAttribArray(ATTRIB_TEXTURE_POSITION);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
  glDeleteVertexArrays(1, &vao);
  glDeleteBuffers(2, vbo);

  return absl::OkStatus();
}

absl::Status SquareOverlayCalculator::GlTeardown() {
  if (program_) {
    glDeleteProgram(program_);
    program_ = 0;
  }
  return absl::OkStatus();
}

}  // namespace mediapipe
