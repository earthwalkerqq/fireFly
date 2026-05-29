APP_NAME := firefly

CC       ?= cc
CXX      ?= c++
CFLAGS   ?= -Wall -Wextra -std=c11 -O2
THREAD_FLAGS ?= -pthread
CPPFLAGS := -Iinclude
CFLAGS += $(THREAD_FLAGS)
LDFLAGS  :=
LDLIBS   := $(THREAD_FLAGS)

SRC_DIR := src
OBJ_DIR := build

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

PKG_CONFIG ?= pkg-config
PKG_PACKAGES := glfw3 glew cglm

HAVE_PKGCONFIG := $(shell command -v $(PKG_CONFIG) >/dev/null 2>&1 && echo 1 || echo 0)

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  GLFW_PREFIX ?= /opt/homebrew/Cellar/glfw/3.4
  GLEW_PREFIX ?= /opt/homebrew/Cellar/glew/2.3.1
  CGLM_PREFIX ?= /opt/homebrew/Cellar/cglm/0.9.6

  CPPFLAGS += -I$(GLFW_PREFIX)/include \
              -I$(GLEW_PREFIX)/include \
              -I$(CGLM_PREFIX)/include \
              -I.

  LDFLAGS  += -L$(GLFW_PREFIX)/lib \
              -L$(GLEW_PREFIX)/lib \
              -L$(CGLM_PREFIX)/lib

  LDLIBS   += -lglfw -lGLEW -lcglm \
              -framework OpenGL -framework Cocoa -framework IOKit \
              -framework CoreVideo -lm -framework OpenCL
else ifeq ($(UNAME_S),Linux)
  ifeq ($(HAVE_PKGCONFIG),1)
    PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKG_PACKAGES) 2>/dev/null)
    PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKG_PACKAGES) 2>/dev/null)

    CFLAGS += $(PKG_CFLAGS)
    LDLIBS += $(PKG_LIBS) -lm
  else
    LDLIBS += -lglfw -lGLEW -lGL -lcglm -lm
  endif
else
  LDLIBS += -lglfw -lGLEW -lGL -lcglm -lm
endif

# --- опциональная поддержка OpenCL: сборка командой `make OPENCL=1` ---
# По умолчанию выключена: проект собирается и работает без OpenCL SDK,
# вычисления при этом выполняются на CPU (pthreads).
OPENCL ?= 0
ifeq ($(OPENCL),1)
  CPPFLAGS += -DUSE_OPENCL
  ifeq ($(UNAME_S),Darwin)
    LDLIBS += -framework OpenCL
  else
    LDLIBS += -lOpenCL
  endif
endif

.PHONY: all clean run

all: $(APP_NAME)

$(APP_NAME): $(OBJ_DIR) $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

run: $(APP_NAME)
	./$(APP_NAME)

clean:
	rm -rf $(OBJ_DIR) $(APP_NAME)
