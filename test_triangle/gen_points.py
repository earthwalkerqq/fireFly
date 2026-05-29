#!/usr/bin/env python3
"""
Генерация точек для теста триангуляции Делоне:
квадратная пластина 100×100 с круглым отверстием радиуса 20 в центре.

Точки расставляются:
  1) по внешнему контуру пластины (равномерно);
  2) по контуру отверстия (равномерно);
  3) узкое кольцо точек чуть снаружи отверстия (плавный переход);
  4) внутренние точки структурной сеткой, исключая буферную зону
     вокруг отверстия и углов.

Результат пишется в points.txt — одна точка на строку «x y».
"""
import math

PLATE   = 100.0
HX, HY  = 50.0, 50.0
HR      = 20.0
HBUFFER = 3.0     # буфер вокруг отверстия для внутренних точек

def near_hole(x, y, buf=HBUFFER):
    return math.hypot(x - HX, y - HY) < HR + buf

pts = []

# 1) Внешний контур пластины: 11 точек на сторону (без дублирования углов)
N_side = 11
xs = [i * PLATE / (N_side - 1) for i in range(N_side)]
# нижняя
for v in xs[:-1]:           pts.append((v, 0.0))
# правая (от нижнего угла к верхнему)
for v in xs[:-1]:           pts.append((PLATE, v))
# верхняя (обратно)
for v in reversed(xs[1:]):  pts.append((v, PLATE))
# левая (обратно)
for v in reversed(xs[1:]):  pts.append((0.0, v))

# 2) Контур отверстия — 20 точек равномерно
N_hole = 20
for i in range(N_hole):
    a = 2 * math.pi * i / N_hole
    pts.append((HX + HR * math.cos(a), HY + HR * math.sin(a)))

# 3) Узкое кольцо снаружи отверстия — 24 точки чуть внахлёст с (2)
N_ring = 24
for i in range(N_ring):
    a = 2 * math.pi * (i + 0.5) / N_ring
    pts.append((HX + (HR + 4.0) * math.cos(a), HY + (HR + 4.0) * math.sin(a)))

# 4) Внутренние точки структурной сеткой с шагом 8
step = 8.0
y = step
while y < PLATE - step / 2:
    x = step
    while x < PLATE - step / 2:
        if not near_hole(x, y) and \
           step * 0.6 < x < PLATE - step * 0.6 and \
           step * 0.6 < y < PLATE - step * 0.6:
            pts.append((x, y))
        x += step
    y += step

# уникализация (на случай мелких совпадений)
seen, unique = set(), []
for p in pts:
    key = (round(p[0], 4), round(p[1], 4))
    if key not in seen:
        seen.add(key)
        unique.append(p)

with open("points.txt", "w") as f:
    for x, y in unique:
        f.write(f"{x:.6f} {y:.6f}\n")

print(f"generated {len(unique)} points -> points.txt")
