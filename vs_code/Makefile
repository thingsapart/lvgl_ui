# Makefile for the LVGL UI VSCode Server

# Compiler and flags
CC = gcc
# Note: -I.. allows including files from the project root.
# The LV_CONF_PATH is crucial for LVGL configuration.
LV_CONF_PATH = ../viewer/lv_conf.h
CFLAGS = -Wall -Wextra -g -I.. -I../viewer -I../cJSON -I../libs -I. -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 -DLV_LVGL_H_INCLUDE_SIMPLE -I../lvgl -DLV_CONF_PATH='"$(LV_CONF_PATH)"' -DWARN_PLAIN_TEXT

# Define LVGL build paths, relative to the vs_code directory
LVGL_DIR = ../lvgl
LVGL_BUILD_DIR = $(LVGL_DIR)/build
LVGL_LIB = $(LVGL_BUILD_DIR)/lib/liblvgl.a

# Linker flags now include the LVGL static library
LDFLAGS = -lm $(LVGL_LIB)

SDL_LIBS = `pkg-config sdl2 -libs`
SDL_CFLAGS = `pkg-config sdl2 -cflags`

# Find all .c files in the parent directory and its subdirectories
# Exclude the original main.c and the sdl_viewer.c as they are for a different target.
SRCS_BASE := $(wildcard ../*.c)
SRCS_BASE := $(filter-out ../main.c, $(SRCS_BASE))
SRCS_VIEWER := $(wildcard ../viewer/*.c)
SRCS_VIEWER := $(filter-out ../viewer/sdl_viewer.c, $(SRCS_VIEWER))
SRCS_CJSON := ../cJSON/cJSON.c
SRCS_GEN_DISPATCH := ../c_gen/lvgl_dispatch.c

# Source files for our new server target
SRCS_SERVER = server/main_vsc.c

# Combine all sources. LVGL sources are no longer listed here.
SRCS = $(SRCS_BASE) $(SRCS_VIEWER) $(SRCS_CJSON) $(SRCS_GEN_DISPATCH) $(SRCS_SERVER)

# Generate object file names from source files
OBJS = $(SRCS:.c=.o)

# Target executable name
TARGET = lvgl_vsc_server

all: $(TARGET)

# Main target rule now depends on the LVGL library being built.
$(TARGET): $(OBJS) $(LVGL_LIB)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS) $(SDL_LIBS)

# Rule to build the LVGL static library if it doesn't exist or is older than its sources.
# This assumes CMake is installed and in the PATH.
$(LVGL_LIB):
	@echo "--- Building LVGL static library ---"
	@echo "LVGL library not found or out of date. Building it..."
	cd .. && ./build_lvgl.sh

# Rule to compile .c files into .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET)
	find .. -name "*.o" -delete 2>/dev/null || true
	find . -name "*.o" -delete 2>/dev/null || true
	# To force a full rebuild of LVGL, uncomment the following line
	# rm -rf $(LVGL_BUILD_DIR)

.PHONY: all clean
