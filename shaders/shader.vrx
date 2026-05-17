#version 330 core

layout (location = 0) in vec3 VertexPos;
layout (location = 1) in float CurHeight;

out float ReliefHeight;
out float PointHeight;

uniform float src_aspect;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

float getPointSize(float hR, float hP) {
  if (hR < 0) return 2.2;
  if (hP < 0) return 2.3;
  float h = abs(hP - hR);
  if (h < 3.) return 2.;
  if (h < 4.) return 2.2;
  if (h < 5.) return 2.5;
  if (h < 6.) return 2.6;
  if (h < 7.) return 2.7;
  if (h < 8.) return 2.8;
  if (h < 9.) return 2.9;
  if (h < 10.) return 3.;
  if (h < 12.) return 3.1;
  if (h < 15.) return 3.2;
  if (h < 18.) return 3.4;
  if (h < 20.) return 3.5;
  if (h < 30.) return 3.8;
  if (h < 40.) return 4.;
  return 5.;
}

void main(void) {
  gl_Position = projection * view * model * vec4(VertexPos, 1.0);
  gl_PointSize = getPointSize(VertexPos.z, CurHeight);
  ReliefHeight = CurHeight;
  PointHeight = VertexPos.z;
}
