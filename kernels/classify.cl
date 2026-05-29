/*
 * Ядро классификации треугольников по критериям безопасности.
 *
 * Логика повторяет предикат isCorrectTriangle_local из src/tlo.c:
 * треугольник считается корректным (пригодным), если высота рельефа
 * во всех его вершинах определена, превышение каждой вершины над
 * рельефом не больше предела, а уклон не превышает заданного значения.
 *
 * Раскладка входных данных (плоские массивы):
 *   pts — по 4 значения float на точку: x, y, z, height
 *         (height — высота опорного рельефа в позиции точки);
 *   tri — по 3 значения int на треугольник: индексы вершин.
 *
 * Результат out[i]: 1 — треугольник корректен, 0 — некорректен.
 */
__kernel void classify_triangles(__global const float *pts,
                                 __global const int   *tri,
                                 const int   countTri,
                                 const float maxHeight,
                                 const float maxSlope,
                                 const float flatTol,
                                 __global uchar *out)
{
    int i = get_global_id(0);
    if (i >= countTri) return;

    int a = tri[i * 3];
    int b = tri[i * 3 + 1];
    int c = tri[i * 3 + 2];

    float ax = pts[a*4], ay = pts[a*4+1], az = pts[a*4+2], ah = pts[a*4+3];
    float bx = pts[b*4], by = pts[b*4+1], bz = pts[b*4+2], bh = pts[b*4+3];
    float cx = pts[c*4], cy = pts[c*4+1], cz = pts[c*4+2], ch = pts[c*4+3];

    /* высота рельефа во всех вершинах должна быть определена */
    if (ah < 0.0f || bh < 0.0f || ch < 0.0f) { out[i] = (uchar)0; return; }

    /* превышение точки над рельефом не больше предельного */
    if (az - ah > maxHeight || bz - bh > maxHeight || cz - ch > maxHeight) {
        out[i] = (uchar)0;
        return;
    }

    /* перепад высот по координате z */
    float maxZ = fmax(az, fmax(bz, cz));
    float minZ = fmin(az, fmin(bz, cz));

    /* квадрат длины наименьшего ребра в плановой проекции XY */
    float d12 = (ax - bx) * (ax - bx) + (ay - by) * (ay - by);
    float d23 = (bx - cx) * (bx - cx) + (by - cy) * (by - cy);
    float d31 = (cx - ax) * (cx - ax) + (cy - ay) * (cy - ay);
    float minD2 = fmin(d12, fmin(d23, d31));
    if (minD2 <= 0.0f) { out[i] = (uchar)0; return; }

    /* перепад высот в пределах шума измерений — треугольник ровный */
    float dz = maxZ - minZ;
    if (dz <= flatTol) { out[i] = (uchar)1; return; }

    /* уклон = перепад высот / длина наименьшего ребра (тангенс угла) */
    float slope = dz / sqrt(minD2);
    out[i] = (slope < maxSlope) ? (uchar)1 : (uchar)0;
}
