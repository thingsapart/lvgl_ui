# Makefile for the CNC Example Application

# --- Configuration ---
PARENT_DIR = ..

# --- Executable Targets ---
NATIVE_TARGET = cnc_app_native
RENDERER_TARGET = cnc_app_renderer

# --- Compiler and Flags ---
CC = gcc
# Note: We do NOT define __DEV_MODE__ so debug logs are disabled for native build.
# We DO define it for the renderer build for easier debugging.
NATIVE_CFLAGS = -Wall -g -std=c11 -O2 -DNDEBUG -I$(PARENT_DIR)
RENDERER_CFLAGS = -Wall -g -std=c11 -D__DEV_MODE__ -I$(PARENT_DIR)

# --- LVGL and SDL Configuration ---
LVGL_DIR = $(PARENT_DIR)/lvgl
LVGL_BUILD_DIR = $(LVGL_DIR)/build
LVGL_LIB = $(LVGL_BUILD_DIR)/lib/liblvgl.a
LV_CONF_PATH = $(PARENT_DIR)/viewer/lv_conf.h

# --- Include Paths ---
INC_CJSON = -I$(PARENT_DIR)/cJSON
LVGL_INC = -I$(LVGL_DIR)
NATIVE_CFLAGS += $(INC_CJSON) -I./c_gen $(LVGL_INC) -DLV_CONF_PATH='"$(LV_CONF_PATH)"'
RENDERER_CFLAGS += $(INC_CJSON) -I$(PARENT_DIR)/c_gen $(LVGL_INC) -DLV_CONF_PATH='"$(LV_CONF_PATH)"'

# --- SDL libs and flags from pkg-config ---
SDL_LIBS = $(shell pkg-config --libs sdl2)
SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
NATIVE_CFLAGS += $(SDL_CFLAGS)
RENDERER_CFLAGS += $(SDL_CFLAGS)

# --- Source Files ---
# VPATH allows make to find sources in parent directories
VPATH = $(PARENT_DIR):$(PARENT_DIR)/viewer:$(PARENT_DIR)/c_gen

# Application-specific sources (local)
NATIVE_APP_SOURCES = main_native.c cnc_app.c
RENDERER_APP_SOURCES = main_renderer.c cnc_app.c

# Shared library sources from parent (needed by both targets)
SHARED_SOURCES_FROM_PARENT = \
	data_binding.c \
	utils.c \
	debug_log.c \
	cJSON/cJSON.c \
	viewer/sdl_viewer.c

# Renderer-only sources from parent
RENDERER_ONLY_SOURCES_FROM_PARENT = \
    api_spec.c ir.c registry.c generator.c \
    yaml_parser.c lvgl_renderer.c viewer/view_inspector.c warning_printer.c \
    c_gen/lvgl_dispatch.c

# Generated UI sources
C_GEN_DIR = ./c_gen
GENERATED_UI_SOURCE = $(C_GEN_DIR)/create_ui.c
GENERATED_UI_HEADER = $(C_GEN_DIR)/create_ui.h

# --- Object File Lists ---
NATIVE_APP_OBJS = $(NATIVE_APP_SOURCES:.c=.o)
RENDERER_APP_OBJS = $(RENDERER_APP_SOURCES:.c=.o)
SHARED_OBJS = $(SHARED_SOURCES_FROM_PARENT:.c=.o)
RENDERER_ONLY_OBJS = $(RENDERER_ONLY_SOURCES_FROM_PARENT:.c=.o)
GENERATED_UI_OBJ = $(GENERATED_UI_SOURCE:.c=.o)

# --- Libraries ---
LIBS = -L$(LVGL_BUILD_DIR)/lib -llvgl $(SDL_LIBS) -lm

# --- Targets ---

.PHONY: all clean run_renderer run_native

# Default target builds the native application
all: $(NATIVE_TARGET)

# --- Target A: Run with dynamic renderer ---
run_renderer: $(RENDERER_TARGET)
	@echo "--- Running CNC example (Renderer Mode) ---"
	./$(RENDERER_TARGET)

# --- Target B: Build and run the native application ---
run_native: $(NATIVE_TARGET)
	@echo "--- Running fully compiled native CNC application ---"
	./$(NATIVE_TARGET)

# --- Build Rules ---

# Rule for the native target
$(NATIVE_TARGET): $(NATIVE_APP_OBJS) $(GENERATED_UI_OBJ) $(SHARED_OBJS) $(LVGL_LIB)
	@echo "--- Linking native CNC application: $(NATIVE_TARGET) ---"
	$(CC) $(NATIVE_CFLAGS) -o $(NATIVE_TARGET) $^ $(LIBS)

# Rule for the renderer target
$(RENDERER_TARGET): $(RENDERER_APP_OBJS) $(SHARED_OBJS) $(RENDERER_ONLY_OBJS) $(LVGL_LIB)
	@echo "--- Linking CNC renderer application: $(RENDERER_TARGET) ---"
	$(CC) $(RENDERER_CFLAGS) -o $(RENDERER_TARGET) $^ $(LIBS)

# --- Prerequisite & Compilation Rules ---

# Build LVGL library if it doesn't exist
$(LVGL_LIB):
	@echo "LVGL library not found. Building it..."
	@$(PARENT_DIR)/build_lvgl.sh

# Generate C code from YAML if sources are missing or ui.yaml is newer
$(GENERATED_UI_SOURCE) $(GENERATED_UI_HEADER): ui.yaml $(PARENT_DIR)/lvgl_ui_generator
	@echo "--- Generating C code from ui.yaml ---"
	@mkdir -p $(C_GEN_DIR)
	$(PARENT_DIR)/lvgl_ui_generator $(PARENT_DIR)/data/api_spec.json ./ui.yaml --codegen c_code > $(GENERATED_UI_SOURCE)
	@echo "// Generated header\n#ifndef CREATE_UI_H\n#define CREATE_UI_H\n#include \"lvgl.h\"\nvoid create_ui(lv_obj_t* parent);\n#endif" > $(GENERATED_UI_HEADER)

# Generic rule to compile .c to .o for native build
$(NATIVE_APP_OBJS) $(GENERATED_UI_OBJ): %.o: %.c
	$(CC) $(NATIVE_CFLAGS) -c $< -o $@

# Generic rule to compile .c to .o for renderer build
$(RENDERER_APP_OBJS): %.o: %.c
	$(CC) $(RENDERER_CFLAGS) -c $< -o $@

# Generic rule for shared sources (can use either CFLAGS, let's pick renderer for dev builds)
$(SHARED_OBJS) $(RENDERER_ONLY_OBJS): %.o: %.c
	$(CC) $(RENDERER_CFLAGS) -c $< -o $@

# Cleanup
clean:
	@echo "Cleaning CNC example directory..."
	rm -f $(NATIVE_TARGET) $(RENDERER_TARGET) *.o
	rm -rf $(C_GEN_DIR)

