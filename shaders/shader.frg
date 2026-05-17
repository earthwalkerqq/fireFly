#version 330 core

in float ReliefHeight;
in float PointHeight;

out vec4 FragColor;

uniform int u_renderMode; // 1 = облако точек, 2 = каркас (wireframe) по высоте
uniform int u_wireOverride;      // 0 = обычный каркас, 1 = принудительный цвет
uniform vec3 u_wireOverrideColor; // цвет для безопасных зон

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

vec3 getColorWireframe(float hR, float hP) {
  float h = hP - hR;
  if (h < 0.0)  return vec3(0.55, 0.40, 0.52);
  if (h < 3.0)  return mix(vec3(0.50, 0.35, 0.48), vec3(0.58, 0.45, 0.52), h / 3.0);
  if (h < 6.0)  return mix(vec3(0.58, 0.45, 0.52), vec3(0.65, 0.45, 0.40), (h - 3.0) / 3.0);
  if (h < 10.0) return mix(vec3(0.65, 0.45, 0.40), vec3(0.70, 0.50, 0.38), (h - 6.0) / 4.0);
  if (h < 18.0) return mix(vec3(0.70, 0.50, 0.38), vec3(0.82, 0.55, 0.35), (h - 10.0) / 8.0);
  if (h < 30.0) return mix(vec3(0.82, 0.55, 0.35), vec3(0.88, 0.65, 0.40), (h - 18.0) / 12.0);
  return vec3(0.90, 0.72, 0.45);
}

void main(void) {
  if (u_renderMode == 1) {
    FragColor = vec4(getColorPoint(ReliefHeight, PointHeight) * 0.85, 1.0);
  } else {
    vec3 wire = (u_wireOverride == 1 ? u_wireOverrideColor
                                     : getColorWireframe(ReliefHeight, PointHeight)) * 0.72;
    FragColor = vec4(wire, 1.0);
  }
}
