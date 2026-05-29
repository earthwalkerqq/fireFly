#include "opencl.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef USE_OPENCL

#ifdef __APPLE__
  #include <OpenCL/opencl.h>
#else
  #include <CL/cl.h>
#endif

#define PATH_KERNEL_CLASSIFY "./kernels/classify.cl"
#define KERNEL_CLASSIFY_NAME "classify_triangles"

/* Состояние гетерогенной вычислительной системы. */
static cl_platform_id   g_platform   = NULL;
static cl_device_id     g_device     = NULL;
static cl_context       g_context    = NULL;
static cl_command_queue g_queue      = NULL;
static cl_program       g_program    = NULL;  /* программа классификации   */
static cl_kernel        g_classify   = NULL;  /* ядро классификации        */
static int              g_ready      = 0;     /* флаг прошла ли инициализация */

/**
 * @details Если в функцию чтения ядра не передать дискриптор буфера, то
  происходит расчет символов файла ядра для последующего выделения определенного
  количества памяти
*/
static void readKernelSource(const char* src, char* buffer, size_t* buf_size, cl_int* err) {
  *err = 0;
  FILE *fd = NULL;
  if (!(fd = fopen(src, "rb"))) {
    fprintf(stderr, "[opencl] не удалось открыть файл ядра %s\n", src);
    *err = 1;
    return;
  }

  if (!buffer) {
    fseek(fd, 0, SEEK_END);
    *buf_size = ftell(fd);
    fclose(fd); fd = NULL;
  } else if (!(*buf_size)) {
    fprintf(stderr, "[opencl] размер передаваемого буфера нулевой\n");
    *err = 1;
    fclose(fd); fd = NULL;
  } else {
    if (fread(buffer, 1, *buf_size, fd) != *buf_size) {
      fprintf(stderr, "[opencl] файл ядра %s прочитан не полностью\n", src);
      *err = 1;
      fclose(fd); fd = NULL;
    }
  }
}


/**
 * @details Поиск устройств идет по всем платформам и в приоритете стоит 
 найти GPU, но если таковых нет, то берется CPU.
 */
static int pickDevice(cl_platform_id *outPlatform, cl_device_id *outDevice) {
  cl_uint numPlatforms;
  if (clGetPlatformIDs(0, NULL, &numPlatforms) != CL_SUCCESS ||
      numPlatforms == 0) {
    fprintf(stderr, "[opencl] платформы OpenCL не найдены\n");
    return 0;
  }

  cl_platform_id platforms[numPlatforms];
  clGetPlatformIDs(numPlatforms, platforms, NULL);

  cl_uint numDevicesType = 2;
  const cl_device_type device_types[2] = { CL_DEVICE_TYPE_GPU, CL_DEVICE_TYPE_CPU };
  for (cl_uint d = 0; d < numDevicesType; ++d) {
    for (cl_uint p = 0; p < numPlatforms; ++p) {
      cl_uint numDevices;
      if (clGetDeviceIDs(platforms[p], device_types[d], 0, NULL, &numDevices)
              == CL_SUCCESS && numDevices > 0) {
        cl_device_id dev = NULL;
        if (clGetDeviceIDs(platforms[p], device_types[d], 1, &dev, NULL)
                == CL_SUCCESS) {
          *outPlatform = platforms[p];
          *outDevice = dev;
          #ifdef DEBUG
          printf("[opencl] Устройство найдено\n");
          #endif
          return 1;
        }
      }
    }
  }

  fprintf(stderr, "[opencl] подходящее устройство не найдено\n");
  return 0;
}


int openclAvailable(void) {
  return 1;
}

int openclInit(void) {
  if (g_ready)
    return 1;

  if (!pickDevice(&g_platform, &g_device))
    return 0;

  cl_int err = CL_SUCCESS;

  g_context = clCreateContext(NULL, 1, &g_device, NULL, NULL, &err);
  if (!g_context || err != CL_SUCCESS) {
    fprintf(stderr, "[opencl] не удалось создать контекст (код %d)\n", err);
    openclShutdown();
    return 0;
  }

  g_queue = clCreateCommandQueue(g_context, g_device, 0, &err);
  if (!g_queue || err != CL_SUCCESS) {
    fprintf(stderr, "[opencl] не удалось создать очередь команд (код %d)\n", err);
    openclShutdown();
    return 0;
  }

  size_t buf_size;
  readKernelSource(PATH_KERNEL_CLASSIFY, NULL, &buf_size, &err);
  if (err) {
    openclShutdown();
    return 0;
  }
  char buf[buf_size];
  readKernelSource(PATH_KERNEL_CLASSIFY, buf, &buf_size, &err);
  if (err) {
    openclShutdown();
    return 0;
  }

  const char* src = buf;
  g_program = clCreateProgramWithSource(g_context, 1, &src, &buf_size, &err);
  if (!g_program || err != CL_SUCCESS) {
    fprintf(stderr, "[opencl] не удалось создать программу (код %d)\n", err);
    openclShutdown();
    return 0;
  }

  err = clBuildProgram(g_program, 1, &g_device, "", NULL, NULL);
  if (err != CL_SUCCESS) {
    size_t logSize = 0;
    clGetProgramBuildInfo(g_program, g_device, CL_PROGRAM_BUILD_LOG,
                          0, NULL, &logSize);
    char *log = (char *)malloc(logSize + 1);
    if (log) {
      clGetProgramBuildInfo(g_program, g_device, CL_PROGRAM_BUILD_LOG,
                            logSize, log, NULL);
      log[logSize] = '\0';
      fprintf(stderr, "[opencl] ошибка сборки ядра:\n%s\n", log);
      free(log);
    }
    openclShutdown();
    return 0;
  }

  g_classify = clCreateKernel(g_program, KERNEL_CLASSIFY_NAME, &err);
  if (!g_classify || err != CL_SUCCESS) {
    fprintf(stderr, "[opencl] не удалось создать ядро %s (код %d)\n",
            KERNEL_CLASSIFY_NAME, err);
    openclShutdown();
    return 0;
  }

  g_ready = 1;
  return 1;
}

void openclShutdown(void) {
  if (g_classify)    { clReleaseKernel(g_classify);       g_classify    = NULL; }
  if (g_program)     { clReleaseProgram(g_program);       g_program     = NULL; }
  if (g_queue)       { clReleaseCommandQueue(g_queue);    g_queue       = NULL; }
  if (g_context)     { clReleaseContext(g_context);       g_context     = NULL; }
  g_device   = NULL;
  g_platform = NULL;
  g_ready    = 0;
}

void openclPrintDevice(void) {
  if (!g_ready) {
    printf("[opencl] устройство не инициализировано — расчёт на CPU\n");
    return;
  }
  char name[256]   = {0};
  char vendor[256] = {0};
  cl_device_type type = 0;
  cl_uint computeUnits = 0;
  cl_ulong globalMem = 0;

  /*
  * @details Используется для получения детальной инфо-ии конкретном вычислительном устройстве.
    С её помощью можно узнать имя устройства, его тип, объем доступной памяти,
    максимальное количество вычислительных блоков и др

  CL_DEVICE_NAME: Возвращает строку с коммерческим названием устройства (например, "NVIDIA GeForce RTX 4070").
  CL_DEVICE_TYPE: Возвращает тип устройства (CL_DEVICE_TYPE_GPU, CL_DEVICE_TYPE_CPU и т.д.).
  CL_DEVICE_GLOBAL_MEM_SIZE: Возвращает объем глобальной памяти устройства в байтах (тип cl_ulong).
  CL_DEVICE_MAX_COMPUTE_UNITS: Количество вычислительных блоков (ядер/мультипроцессоров) у устройства.
  CL_DEVICE_MAX_WORK_GROUP_SIZE: Максимальное количество рабочих элементов (threads) в одной рабочей группе
  */

  clGetDeviceInfo(g_device, CL_DEVICE_NAME, sizeof(name), name, NULL);
  clGetDeviceInfo(g_device, CL_DEVICE_VENDOR, sizeof(vendor), vendor, NULL);
  clGetDeviceInfo(g_device, CL_DEVICE_TYPE, sizeof(type), &type, NULL);
  clGetDeviceInfo(g_device, CL_DEVICE_MAX_COMPUTE_UNITS,
                  sizeof(computeUnits), &computeUnits, NULL);
  clGetDeviceInfo(g_device, CL_DEVICE_GLOBAL_MEM_SIZE,
                  sizeof(globalMem), &globalMem, NULL);

  const char *typeStr = (type & CL_DEVICE_TYPE_GPU) ? "GPU"
                      : (type & CL_DEVICE_TYPE_CPU) ? "CPU" : "иное";
  printf("[opencl] устройство: %s | %s | тип: %s | "
         "вычислительных блоков: %u | глобальная память: %llu МБ\n",
         name, vendor, typeStr, computeUnits,
         (unsigned long long)(globalMem / (1024ULL * 1024ULL)));
}

int openclClassifyTriangles(const Triangle *tri, size_t countTri,
                            const point_t *pts, size_t countPts,
                            double maxHeight, double maxSlope,
                            double flatTol, unsigned char *out) {
  if (!g_ready || !tri || !pts || !out || countTri == 0 || countPts == 0)
    return 0;

  /* Перепаковка данных в плоские массивы для передачи на устройство. */
  float *ptsFlat = (float *)malloc(countPts * 4 * sizeof(float));
  int   *triFlat = (int *)malloc(countTri * 3 * sizeof(int));
  if (!ptsFlat || !triFlat) {
    free(ptsFlat);
    free(triFlat);
    return 0;
  }
  for (size_t i = 0; i < countPts; i++) {
    ptsFlat[i * 4]     = pts[i].x;
    ptsFlat[i * 4 + 1] = pts[i].y;
    ptsFlat[i * 4 + 2] = pts[i].z;
    ptsFlat[i * 4 + 3] = pts[i].height;
  }
  for (size_t i = 0; i < countTri; i++) {
    triFlat[i * 3]     = tri[i].p1;
    triFlat[i * 3 + 1] = tri[i].p2;
    triFlat[i * 3 + 2] = tri[i].p3;
  }

  cl_int err = CL_SUCCESS;
  int ok = 0;
  cl_mem bufPts = NULL, bufTri = NULL, bufOut = NULL;

  bufPts = clCreateBuffer(g_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                          countPts * 4 * sizeof(float), ptsFlat, &err);
  if (err != CL_SUCCESS) {
    if (bufPts) clReleaseMemObject(bufPts);
    if (bufTri) clReleaseMemObject(bufTri);
    if (bufOut) clReleaseMemObject(bufOut);
    free(ptsFlat);
    free(triFlat);
    if (!ok)
      fprintf(stderr, "[opencl] ошибка выполнения ядра (код %d)\n", err);
    return ok;
  }

  bufTri = clCreateBuffer(g_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                          countTri * 3 * sizeof(int), triFlat, &err);
  if (err != CL_SUCCESS) {
    if (bufPts) clReleaseMemObject(bufPts);
    if (bufTri) clReleaseMemObject(bufTri);
    if (bufOut) clReleaseMemObject(bufOut);
    free(ptsFlat);
    free(triFlat);
    if (!ok)
      fprintf(stderr, "[opencl] ошибка выполнения ядра (код %d)\n", err);
    return ok;
  }

  bufOut = clCreateBuffer(g_context, CL_MEM_WRITE_ONLY,
                          countTri * sizeof(unsigned char), NULL, &err);
  if (err != CL_SUCCESS) {
    if (bufPts) clReleaseMemObject(bufPts);
    if (bufTri) clReleaseMemObject(bufTri);
    if (bufOut) clReleaseMemObject(bufOut);
    free(ptsFlat);
    free(triFlat);
    if (!ok)
      fprintf(stderr, "[opencl] ошибка выполнения ядра (код %d)\n", err);
    return ok;
  }

  cl_int   ctArg = (cl_int)countTri;
  cl_float mhArg = (cl_float)maxHeight;
  cl_float msArg = (cl_float)maxSlope;
  cl_float ftArg = (cl_float)flatTol;

  err  = clSetKernelArg(g_classify, 0, sizeof(cl_mem),   &bufPts);
  err |= clSetKernelArg(g_classify, 1, sizeof(cl_mem),   &bufTri);
  err |= clSetKernelArg(g_classify, 2, sizeof(cl_int),   &ctArg);
  err |= clSetKernelArg(g_classify, 3, sizeof(cl_float), &mhArg);
  err |= clSetKernelArg(g_classify, 4, sizeof(cl_float), &msArg);
  err |= clSetKernelArg(g_classify, 5, sizeof(cl_float), &ftArg);
  err |= clSetKernelArg(g_classify, 6, sizeof(cl_mem),   &bufOut);
  if (err != CL_SUCCESS) {
    if (bufPts) clReleaseMemObject(bufPts);
    if (bufTri) clReleaseMemObject(bufTri);
    if (bufOut) clReleaseMemObject(bufOut);
    free(ptsFlat);
    free(triFlat);
    if (!ok)
      fprintf(stderr, "[opencl] ошибка выполнения ядра (код %d)\n", err);
    return ok;
  }

  size_t globalSize = countTri;
  err = clEnqueueNDRangeKernel(g_queue, g_classify, 1, NULL,
                               &globalSize, NULL, 0, NULL, NULL);
  if (err != CL_SUCCESS) {
    if (bufPts) clReleaseMemObject(bufPts);
    if (bufTri) clReleaseMemObject(bufTri);
    if (bufOut) clReleaseMemObject(bufOut);
    free(ptsFlat);
    free(triFlat);
    if (!ok)
      fprintf(stderr, "[opencl] ошибка выполнения ядра (код %d)\n", err);
    return ok;
  }

  err = clEnqueueReadBuffer(g_queue, bufOut, CL_TRUE, 0,
                            countTri * sizeof(unsigned char), out,
                            0, NULL, NULL);
  if (err != CL_SUCCESS) {
    if (bufPts) clReleaseMemObject(bufPts);
    if (bufTri) clReleaseMemObject(bufTri);
    if (bufOut) clReleaseMemObject(bufOut);
    free(ptsFlat);
    free(triFlat);
    if (!ok)
      fprintf(stderr, "[opencl] ошибка выполнения ядра (код %d)\n", err);
    return ok;
  }

  ok = 1;
  return ok;
}

#else

int openclAvailable(void) { return 0; }

int openclInit(void) { return 0; }

void openclShutdown(void) {}

void openclPrintDevice(void) {
  printf("[opencl] сборка без поддержки OpenCL — расчёт на CPU (pthreads)\n");
}

int openclClassifyTriangles(const Triangle *tri, size_t countTri,
                            const point_t *pts, size_t countPts,
                            double maxHeight, double maxSlope,
                            double flatTol, unsigned char *out) {
  (void)tri; (void)countTri; (void)pts; (void)countPts;
  (void)maxHeight; (void)maxSlope; (void)flatTol; (void)out;
  return 0;
}

#endif
