#include <stdio.h>
#include <stdlib.h>

#define GL_SILENCE_DEPRECATION 1

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cglm/cglm.h>
#include <math.h>

#include "common.h"
#include "rzp.h"
#include "tlo.h"
#include "shader.h"
#include "triangulation.h"

extern char drawMode;

#define DEBUG

#define WIN_WIDTH 1248
#define WIN_HEIGHT 1024

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

vec3 cameraPos = {0.f, 0.f, 0.f};
vec3 cameraFront = {0.f, 1.f, 0.f};  // направление взгляда (по оси Y)
vec3 cameraUp = {0.f, 0.f, 1.f};     // направление вверх (ось Z)

static float pitch = 0.f; // вертикальный поворот
static float yaw = -90.f; // горизонтальный поворот

static float cameraSpeed = 0.5f;

static GLFWwindow* _glfwGetWindow(int win_width, int win_height) {
  if (!glfwInit()) {
    fprintf(stderr, "FAILED FROM INITIALIZE GLFW\n");
    return NULL;
  }
  // opengl 3.3
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  GLFWwindow *window = glfwCreateWindow(win_width, win_height, "FireFly", glfwGetPrimaryMonitor(), NULL);
  if (!window) {
    fprintf(stderr, "FAILED FROM CREATE A WINDOW\n");
    glfwTerminate();
    return NULL;
  }

  glfwMakeContextCurrent(window);

  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
  return window;
}

static void _glSetup(void) {
  glClearColor(0.2f, 0.2f, 0.2f, 1.f);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glFrontFace(GL_CCW);
  glDisable(GL_BLEND);
  glShadeModel(GL_SMOOTH);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glfwSwapInterval(1);
}

void keyCallback(GLFWwindow* window, float deltaTime) {
    float currentSpeed = 20 * cameraSpeed * deltaTime;

    if (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS) {
      drawMode = 1;
    } 
    if (glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS) {
      drawMode = 2;
    }

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        vec3 resMul;
        glm_vec3_scale(cameraFront, currentSpeed, resMul);
        glm_vec3_add(cameraPos, resMul, cameraPos);
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        vec3 resMul;
        glm_vec3_scale(cameraFront, currentSpeed, resMul);
        glm_vec3_sub(cameraPos, resMul, cameraPos);
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        vec3 res;
        glm_vec3_cross(cameraFront, cameraUp, res);
        vec3 normVec;
        glm_vec3_normalize_to(res, normVec);
        vec3 resMul;
        glm_vec3_scale(normVec, currentSpeed, resMul);
        glm_vec3_sub(cameraPos, resMul, cameraPos);
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        vec3 res;
        glm_vec3_cross(cameraFront, cameraUp, res);
        vec3 normVec;
        glm_vec3_normalize_to(res, normVec);
        vec3 resMul;
        glm_vec3_scale(normVec, currentSpeed, resMul);
        glm_vec3_add(cameraPos, resMul, cameraPos);
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) {
        vec3 resMul;
        glm_vec3_scale(cameraUp, currentSpeed, resMul);
        glm_vec3_add(cameraPos, resMul, cameraPos);
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) {
        vec3 resMul;
        glm_vec3_scale(cameraUp, currentSpeed, resMul);
        glm_vec3_sub(cameraPos, resMul, cameraPos);
    }
    if (glfwGetKey(window, GLFW_KEY_O) == GLFW_PRESS) {
      yaw += 0.5f;
    }
    if (glfwGetKey(window, GLFW_KEY_P) == GLFW_PRESS) {
      yaw -= 0.5f;
    }
    if (glfwGetKey(window, GLFW_KEY_K) == GLFW_PRESS) {
      pitch -= 0.5f;
    }
    if (glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
      pitch += 0.5f;
    }
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS ||
        glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GL_TRUE);
    }

    if (pitch < -89.f) pitch = -89.f;
    else if (pitch > 89.f) pitch = 89.f;

    // вычисляем направление взгляда
    // горизонталь: плоскость X-Y, высота: Z
    vec3 front;
    front[0] = cosf(glm_rad(yaw)) * cosf(glm_rad(pitch)); // X
    front[1] = sinf(glm_rad(yaw)) * cosf(glm_rad(pitch)); // Y
    front[2] = sinf(glm_rad(pitch));                      // Z
    glm_vec3_normalize_to(front, cameraFront);
}

void mouseCallback(GLFWwindow* window, int button, int action, int __attribute__((unused)) mods) {
    static int cursor_value = GLFW_CURSOR_NORMAL;

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        cursor_value = (cursor_value == GLFW_CURSOR_HIDDEN) ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN;
        glfwSetInputMode(window, GLFW_CURSOR, cursor_value);
    }
}

void mainloop(GLFWwindow* window, TloRender* tlo, GLuint shaderProg) {
  float lastFrame = glfwGetTime();
  float deltaFrame;
  while (!glfwWindowShouldClose(window)) {
    float curFrame = glfwGetTime();
    deltaFrame = curFrame - lastFrame;
    lastFrame = curFrame;

    keyCallback(window, deltaFrame);

    glClearColor(0.2f, 0.2f, 0.2f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(shaderProg);

    int uni_loc = glGetUniformLocation(shaderProg, "src_aspect");
    int winWidth, winHeight;
    glfwGetWindowSize(window, &winWidth, &winHeight);
    glUniform1f(uni_loc, (float)(winHeight) / winWidth);

    mat4 model;
    glm_mat4_identity(model);

    mat4 view;
    vec3 center;
    glm_vec3_add(cameraPos, cameraFront, center);
    glm_lookat(cameraPos, center, cameraUp, view);

    mat4 projection;
    glm_perspective(glm_rad(70.f), (float)(winWidth) / winHeight, 0.1f, 10000.f, projection);

    int uni_model = glGetUniformLocation(shaderProg, "model");
    int uni_view = glGetUniformLocation(shaderProg, "view");
    int uni_proj = glGetUniformLocation(shaderProg, "projection");
    glUniformMatrix4fv(uni_model, 1, GL_FALSE, &model[0][0]);
    glUniformMatrix4fv(uni_view, 1, GL_FALSE, &view[0][0]);
    glUniformMatrix4fv(uni_proj, 1, GL_FALSE, &projection[0][0]);

    if (!drawTlo(tlo, shaderProg)) return;

    glfwSwapBuffers(window);
    glfwPollEvents();
  }
}

int main(int argc, char** argv) {
  const char *pathData = (argc > 1) ? argv[1] : PATH_DATA;

  GLFWwindow* window = _glfwGetWindow(WIN_WIDTH, WIN_HEIGHT);
  if (!window) return 1;

  glewExperimental = GL_TRUE;
  GLuint glewErr = glewInit();
  if (glewErr != GLEW_OK) {
    fprintf(stderr, "FAILED FROM INITIALIZE GLEW\n");
    glfwTerminate();
    return 1;
  }

  _glSetup();

  int heights[RZP_MATRIX_SIZE];
  if (!getRZPMtrx(PATH_DATA, RZP_TILES[0], heights)) { // пока есть ед-ый файл с rzp
    return 1;
  }

  TloRender tlo;
  char tloPath[512];
  snprintf(tloPath, 512, "%s/TLO_INT", pathData);
  size_t sizeTlo = 0; // размер памяти занимаемый всеми файлами тло
  int countTlo = 0;   // кол-во всех флагментов тло
  if (!initTlo(&tlo, tloPath, &sizeTlo, &countTlo) || !sizeTlo || !countTlo) {
    fprintf(stderr, "FAILED FROM INIT TLO\n");
    glfwTerminate();
    return 1;
  }

  #ifdef DEBUG
  printf("Count fragment tlo = %d\n", countTlo);
  printf("Sizeof all tlo data = %zu\n", sizeTlo);
  #endif

  if (!loadAllTloData(&tlo, tloPath, sizeTlo, countTlo, heights)) {
    fprintf(stderr, "FAILED FROM LOAD DATA\n");
    glfwTerminate();
    return 1;
  }

  glfwSetMouseButtonCallback(window, mouseCallback);

  // позиционируем камеру относительно загруженного облака точек
  float centerX = (tlo.minX + tlo.maxX) * 0.5f;
  float centerY = (tlo.minY + tlo.maxY) * 0.5f;
  float centerZ = (tlo.minZ + tlo.maxZ) * 0.5f;

  cameraPos[0] = centerX;
  cameraPos[1] = centerY;
  cameraPos[2] = centerZ + (tlo.maxZ - tlo.minZ) * 0.5f + 100.0f;

  vec3 lookAt;
  lookAt[0] = centerX;
  lookAt[1] = centerY;
  lookAt[2] = centerZ;
  glm_vec3_sub(lookAt, cameraPos, cameraFront);
  glm_vec3_normalize(cameraFront);

  GLuint shaderProg = createShader(PATH_VRX_SHADER, PATH_FRG_SHADER);
  if (!shaderProg) {
    glfwTerminate();
    return 1;
  }

  drawMode = 1;

  mainloop(window, &tlo, shaderProg);

  freeTlo(&tlo);
  return 0;
}
