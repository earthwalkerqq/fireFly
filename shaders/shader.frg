#version 330 core

in float ReliefHeight;
in float PointHeight;
in vec3  WorldPos;

out vec4 FragColor;

// Проход отрисовки:
//   0 — облако точек (цвет по превышению над рельефом)
//   1 — заливка серой триангуляционной сетки
//   2 — каркас рёбер сетки
//   3 — заливка безопасных зон цветом по рангу пригодности
uniform int  u_pass;
uniform vec3 u_meshColor;  // базовый цвет серой сетки
uniform vec3 u_wireColor;  // цвет рёбер каркаса

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

// Плоское освещение грани: нормаль восстанавливается по экранным
// производным мировой позиции, что даёт чёткую огранку поверхности.
float faceShade()
{
  vec3 n = normalize(cross(dFdx(WorldPos), dFdy(WorldPos)));
  vec3 lightDir = normalize(vec3(0.35, 0.45, 0.82));
  float diff = abs(dot(n, lightDir));
  return clamp(0.55 + 0.5 * diff, 0.0, 1.0);
}

// Цвет зоны по нормированному рангу: 0.0 — лучшая зона (зелёная),
// 1.0 — наименее пригодная (красная), переход через жёлтый.
vec3 rankColor(float t)
{
  t = clamp(t, 0.0, 1.0);
  vec3 best  = vec3(0.16, 0.68, 0.34);
  vec3 mid   = vec3(0.96, 0.78, 0.20);
  vec3 worst = vec3(0.84, 0.21, 0.19);
  if (t < 0.5) return mix(best, mid,  t / 0.5);
  return mix(mid, worst, (t - 0.5) / 0.5);
}

void main(void)
{
  if (u_pass == 0) {
    FragColor = vec4(getColorPoint(ReliefHeight, PointHeight) * 0.9, 1.0);
  } else if (u_pass == 1) {
    FragColor = vec4(u_meshColor * faceShade(), 1.0);
  } else if (u_pass == 2) {
    FragColor = vec4(u_wireColor, 1.0);
  } else {
    FragColor = vec4(rankColor(ReliefHeight) * faceShade(), 1.0);
  }
}
