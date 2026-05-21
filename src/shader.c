#include <stdio.h>
#include <stdlib.h>

#include "shader.h"

#include <GL/glew.h>

static char* readFile(const char* path) {
  FILE* fd = NULL;
  if (!(fd = fopen(path, "rb"))) {
    fprintf(stderr, "FAILED FROM OPEN FILE %s\n", path);
    return NULL;
  }

  fseek(fd, 0, SEEK_END);
  size_t len = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  char* buffer = (char*)malloc(len + 1);
  if (!buffer) {
    fprintf(stderr, "FAILED FROM MEMORY ALLOCATE\n");
    fclose(fd);
    return NULL;
  }

  if (fread(buffer, 1, len, fd) != len) {
    fprintf(stderr, "FAILED FROM READ FILE");
    fclose(fd);
    free(buffer);
    return NULL;
  }

  buffer[len] = '\0';

  fclose(fd);
  return buffer;
}

static GLuint compileShader(const char* src, GLenum type) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, NULL);
  glCompileShader(shader);

  int success;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (!success) {
    char infoLog[512];
    glGetShaderInfoLog(shader, 512, NULL, infoLog);
    fprintf(stderr, "FAILED FROM SHADER COMPILE (%s):\n%s\n", type == GL_VERTEX_SHADER ? "VERTEX" : "FRAGMENT", infoLog);
    return 0;
  }
  return shader;
}

GLuint createShader(const char* vertPath, const char* fragPath) {
  char* shaderVert = readFile(vertPath);
  char* shaderFrag = readFile(fragPath);

  if (!shaderVert || !shaderFrag) {
    free(shaderVert);
    free(shaderFrag);
    return 0;
  }

  GLuint compShaderVert = compileShader(shaderVert, GL_VERTEX_SHADER);
  GLuint compShaderFrag = compileShader(shaderFrag, GL_FRAGMENT_SHADER);

  free(shaderVert);
  free(shaderFrag);
  if (!compShaderVert || !compShaderFrag) return 0;

  GLuint shaderProg = glCreateProgram(); // создает пустой объект шейдерной программы и возвращает его ненулевой идентификатор

  glAttachShader(shaderProg, compShaderFrag);
  glAttachShader(shaderProg, compShaderVert);
  glLinkProgram(shaderProg);

  int success;
  glGetProgramiv(shaderProg, GL_LINK_STATUS, &success);
  if (!success) {
    char infoLog[512];
    glGetProgramInfoLog(shaderProg, 512, NULL, infoLog);
    fprintf(stderr, "FAILED FROM LINKED PROGRAM:\n%s\n", infoLog);
     return 0;
  }

  // glDetachShader(shaderProg, compShader);
  glDeleteShader(compShaderFrag);
  glDeleteShader(compShaderVert);

  return shaderProg;
}
