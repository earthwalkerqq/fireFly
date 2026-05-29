# Сборка автономного теста триангуляции (с использованием cglm + OpenGL для просмотра)
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -pthread
INCLUDE := -I../include

UNAME_S := $(shell uname -s)

# Автоматическое определение флагов cglm/glfw3/glew через pkg-config
# Если pkg-config найдет пакеты, он добавит нужные пути (-I) и библиотеки (-l)
PKG_CFLAGS := $(shell pkg-config --cflags cglm glfw3 glew 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs   cglm glfw3 glew 2>/dev/null || echo "-lcglm -lglfw -lGLEW")

# Платформенно-зависимая линковка OpenGL
ifeq ($(UNAME_S),Darwin)
  # На macOS — pkg-config может отсутствовать; добавляем явные пути Cellar (если нужно)
  GLFW_PREFIX ?= /opt/homebrew/Cellar/glfw/3.4
  GLEW_PREFIX ?= /opt/homebrew/Cellar/glew/2.3.1
  CGLM_PREFIX ?= /opt/homebrew/Cellar/cglm/0.9.6
  INCLUDE += -I$(GLFW_PREFIX)/include -I$(GLEW_PREFIX)/include -I$(CGLM_PREFIX)/include
  LDFLAGS_EXTRA := -L$(GLFW_PREFIX)/lib -L$(GLEW_PREFIX)/lib -L$(CGLM_PREFIX)/lib
  GL_LIBS := -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
else
  LDFLAGS_EXTRA :=
  GL_LIBS := -lGL
endif

# Добавляем флаги пакетов к основным флагам проекта
CFLAGS  += $(PKG_CFLAGS)
LIBS    := $(LDFLAGS_EXTRA) $(PKG_LIBS) $(GL_LIBS) -lm

SRC := test_triangle.c ../src/triangulation.c ../src/opencl.c ../src/common.c

all: test_triangle

test_triangle: $(SRC) ../include/triangulation.h ../include/opencl.h
	$(CC) $(CFLAGS) $(INCLUDE) $(SRC) -o $@ $(LIBS)

points.txt:
	python3 gen_points.py

run: test_triangle points.txt
	./test_triangle

clean:
	rm -f test_triangle result.svg stats.txt

distclean: clean
	rm -f points.txt

.PHONY: all run clean distclean
