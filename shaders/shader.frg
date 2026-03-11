#version 330 core

in float ReliefHeight;
in float PointHeight;

out vec4 FragColor;

uniform int u_renderMode; // 1 = облако точек (цвет по высоте), 2 = серая триангуляция

vec3 getColorPoint(float hR, float hP)
{
  if (hR < 0) return vec3(255.0, 0.0, 0.0) / 255.0;
  if (hP < 0) return vec3(0.0,   0.0, 255.0) / 255.0;
  float h = hP - hR;
  if(h < -1.0) return mix(vec3(255.0, 153.0, 255.0), vec3(153.0, 0.0, 153.0), (h + 1.0) / -(15.0 - 1.0)) / 255.0;
  if (h < 0.0)  return mix(vec3(50.0, 100.0, 0.0), vec3(50.0, 255.0, 0.0), h / 2.9) / 255.0;
  if (h < 3.0)  return mix(vec3(100.0, 100.0, 0.0), vec3(100.0, 255.0, 0.0), h / 2.9) / 255.0;
  if (h < 5.0)  return mix(vec3(200.0, 200.0, 0.0), vec3(255.0, 255.0, 0.0), (h - 3.5) / (4.9 - 1.5)) / 255.0;
  if (h < 10.0) return mix(vec3(255.0, 155.0, 100.0), vec3(255.0, 155.0, 100.0), (h - 5.0) / (10.0 - 5.0)) / 255.0;
  if (h < 20.0) return mix(vec3(255.0, 155.0, 0.0), vec3(255.0, 0.0, 0.0), (h - 10.0) / (15.0 - 5.0)) / 255.0;
  if (h < 40.0) return vec3(255.0, 0.0, 0.0) / 255.0;
  return vec3(255.0, 255.0, 255.0) / 255.0;
}

void main(void) {
  // оба режима — цвет по высоте (красивая раскраска рельефа)

  if (u_renderMode == 1) {
    FragColor = vec4(getColorPoint(ReliefHeight, PointHeight), 1.0);
  } else {
    FragColor = vec4(0.4, 0.4, 0.4, 0.1);
  }

}
