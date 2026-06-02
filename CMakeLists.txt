# ---------------------------------------------------------------------------
#  Сборка проекта firefly через CMake.
#
#
#  Исполняемый файл создаётся в корне проекта, поэтому относительные пути
#  ./shaders и ./kernels разрешаются так же, как при сборке через Makefile.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.13)
project(firefly C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(USE_OPENCL "Сборка с поддержкой OpenCL" ON)
option(DEBUG "Сборка в дебаг режиме" ON)

# Исполняемый файл размещается рядом с каталогами shaders/ и kernels/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

file(GLOB FIREFLY_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.c")

add_executable(firefly ${FIREFLY_SOURCES})
target_include_directories(firefly PRIVATE "${CMAKE_SOURCE_DIR}/include")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(firefly PRIVATE -Wall -Wextra -Wno-nullability-completeness-on-arrays)
endif()

# --- потоки (POSIX threads) ---
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(firefly PRIVATE Threads::Threads)

# --- OpenGL ---
find_package(OpenGL REQUIRED)
target_link_libraries(firefly PRIVATE OpenGL::GL)

# --- GLEW ---
find_package(GLEW REQUIRED)
target_link_libraries(firefly PRIVATE GLEW::GLEW)

# --- GLFW3 ---
find_package(glfw3 QUIET)
if(TARGET glfw)
  target_link_libraries(firefly PRIVATE glfw)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW REQUIRED glfw3)
  target_include_directories(firefly PRIVATE ${GLFW_INCLUDE_DIRS})
  target_link_directories(firefly PRIVATE ${GLFW_LIBRARY_DIRS})
  target_link_libraries(firefly PRIVATE ${GLFW_LIBRARIES})
endif()

# --- cglm (в основном header-only) ---
find_package(cglm QUIET)
if(TARGET cglm)
  target_link_libraries(firefly PRIVATE cglm)
elseif(TARGET cglm::cglm)
  target_link_libraries(firefly PRIVATE cglm::cglm)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(CGLM QUIET cglm)
  endif()
  if(CGLM_FOUND)
    target_include_directories(firefly PRIVATE ${CGLM_INCLUDE_DIRS})
    target_link_directories(firefly PRIVATE ${CGLM_LIBRARY_DIRS})
    target_link_libraries(firefly PRIVATE ${CGLM_LIBRARIES})
  else()
    message(STATUS "firefly: cglm не найден через CMake/pkg-config — "
                   "заголовки берутся из стандартных путей включения")
  endif()
endif()

find_library(MATH_LIB m)
if(MATH_LIB)
  target_link_libraries(firefly PRIVATE ${MATH_LIB})
endif()

if(USE_OPENCL)
  find_package(OpenCL REQUIRED)
  target_compile_definitions(firefly PRIVATE USE_OPENCL)
  target_link_libraries(firefly PRIVATE OpenCL::OpenCL)
  message(STATUS "firefly: поддержка OpenCL включена")
else()
  message(STATUS "firefly: сборка без OpenCL (расчёт на CPU)")
endif()
