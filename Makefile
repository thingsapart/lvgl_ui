CC = gcc

ifneq ("$(wildcard ./cJSON/libcjson.a)","")
    LIB_CJSON = ./cJSON/libcjson.a
    INC_CJSON = -I./cJSON
else
    LIBCJSON = -lcjson
    INC_CJSON = -I/usr/include/cjson
endif

INC_LODE_PNG = -I./libs

# Define the LVGL directory relative to this script
LVGL_DIR = $(PWD)/lvgl
LVGL_BUILD_DIR = $(LVGL_DIR)/build
LV_CONF_PATH = $(LVGL_DIR)/../viewer/lv_conf.h

# Define the path to the LVGL static library
LVGL_LIB = $(LVGL_BUILD_DIR)/lib/liblvgl.a

SDL_LIBS = `pkg-config sdl2 -libs`
SDL_CFLAGS = `pkg-config sdl2 -cflags`

LVGL_INC = -I./lvgl/src

# Set __DEV_MODE__ for debug logging in the main generator tool
CFLAGS = -Wall -g -std=c11 -I. -D__DEV_MODE__ $(INC_CJSON) -I./cJSON -D_GNU_SOURCE -I./lvgl -DLV_CONF_PATH='"$(LV_CONF_PATH)"' -DLV_BUILD_CONF_PATH='"$(LV_CONF_PATH)"' $(SDL_CFLAGS) $(LVGL_INC)
LIBS = $(LIB_CJSON) -lm $(LVGL_LIB) $(SDL_LIBS)

TARGET = lvgl_ui_generator

# Python script for generating lvgl_dispatch.c and .h
API_SPEC_GENERATOR_PY = ./generate_dynamic_lvgl_dispatch.py
LV_DEF_JSON = ./data/lv_def.json
API_SPEC_JSON = ./api_spec.json
DYNAMIC_LVGL_H = ./c_gen/lvgl_dispatch.h
DYNAMIC_LVGL_C = ./c_gen/lvgl_dispatch.c
DYNAMIC_LVGL_O = $(DYNAMIC_LVGL_C:.c=.o)

# Options for the python script
CONSOLIDATION_MODE ?= aggressive # Default to aggressive, can be overridden: make CONSOLIDATION_MODE=typesafe
#CONSOLIDATION_MODE ?= typesafe # Default to typesafe, can be overridden: make CONSOLIDATION_MODE=aggressive
# To enable specific input methods for dynamic_lvgl:
# Override with make DYNAMIC_LVGL_CFLAGS="-DENABLE_CJSON_INPUTS -DENABLE_IR_INPUTS"
DYNAMIC_LVGL_CFLAGS ?= -DENABLE_IR_INPUTS # Default to only IR inputs

# Add DYNAMIC_LVGL_CFLAGS to general CFLAGS
CFLAGS += $(DYNAMIC_LVGL_CFLAGS)

SOURCES = api_spec.c ir.c registry.c generator.c ir_printer.c ir_debug_printer.c c_code_printer.c utils.c debug_log.c cJSON/cJSON.c $(DYNAMIC_LVGL_C) viewer/sdl_viewer.c viewer/view_inspector.c lvgl_renderer.c yaml_parser.c warning_printer.c data_binding.c libs/lodepng.c
OBJECTS = $(SOURCES:.c=.o)

# Main target rule now depends on the LVGL library
$(TARGET): $(OBJECTS) main.o $(LVGL_LIB)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) main.o $(LIBS)

$(OBJECTS): %.o: %.c
	@echo "Compiling $<..."
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to build the LVGL static library if it doesn't exist.
# This rule is triggered because $(LVGL_LIB) is a prerequisite for $(TARGET).
$(LVGL_LIB):
	@echo "LVGL library not found or out of date. Building it..."
	./build_lvgl.sh

$(API_SPEC_JSON) : ./generate_api_spec.py $(LV_DEF_JSON)
	@echo "Generating API Spec: $(API_SPEC_JSON)"
	python3 ./generate_api_spec.py ./data/lv_def.json --lvgl-conf $(LV_CONF_PATH) > $(API_SPEC_JSON)


# Rule to generate dynamic_lvgl.h and dynamic_lvgl.c
# These files depend on the Python script and the api_spec.json
$(DYNAMIC_LVGL_H) $(DYNAMIC_LVGL_C): $(API_SPEC_GENERATOR_PY) $(API_SPEC_JSON)
	@echo "Generating $(DYNAMIC_LVGL_H) and $(DYNAMIC_LVGL_C)"
	python3 $(API_SPEC_GENERATOR_PY) $(API_SPEC_JSON) \
		--header-out $(DYNAMIC_LVGL_H) \
		--source-out $(DYNAMIC_LVGL_C)

# Ensure generated files are created before objects that depend on them are compiled.
# Specifically, main.o or any other .o that might include dynamic_lvgl.h
# and the dynamic_lvgl.o itself.
$(DYNAMIC_LVGL_O): $(DYNAMIC_LVGL_C) $(DYNAMIC_LVGL_H)
main.o: $(DYNAMIC_LVGL_H)

.PHONY: all clean run ex_cnc_rendered ex_cnc_native

all: $(TARGET)

run: $(TARGET)
	./$(TARGET) $(API_SPEC_JSON) ./ui.json

# --- CNC Example Targets ---
TARGET_CNC_RENDERED = ./ex_cnc/ex_cnc_rendered
ex_cnc_rendered: $(TARGET_CNC_RENDERED)
$(TARGET_CNC_RENDERED): $(OBJECTS) $(LVGL_LIB) ex_cnc/cnc_app.o ex_cnc/cnc_main_live.o
	@echo "\n--- Building CNC Example (Renderer) ---\n"
	# $(MAKE) -C ex_cnc run_renderer
	$(CC) $(CFLAGS) -o $(TARGET_CNC_RENDERED) $^ $(LIBS)
	$(TARGET_CNC_RENDERED)

TARGET_CNC_NATIVE = ./ex_cnc/ex_cnc_native
ex_cnc_native: $(TARGET_CNC_NATIVE)
C_GEN_DIR = ./c_gen
GENERATED_UI_SOURCE = $(C_GEN_DIR)/create_ui.c
GENERATED_UI_HEADER = $(C_GEN_DIR)/create_ui.h
GENERATED_UI_OBJ = $(GENERATED_UI_SOURCE:.c=.o)
EX_CNC_UI_YAML = ex_cnc/cnc_ui.yaml
$(GENERATED_UI_SOURCE) $(GENERATED_UI_HEADER): $(EX_CNC_UI_YAML) $(TARGET)
	@echo "--- Generating C code from ui.yaml ---"
	@mkdir -p $(C_GEN_DIR)
	@rm -f  $(GENERATED_UI_SOURCE)
	./$(TARGET) $(API_SPEC_JSON) $(EX_CNC_UI_YAML) --codegen c_code > $(GENERATED_UI_SOURCE)
	@echo "// Generated header\n#ifndef CREATE_UI_H\n#define CREATE_UI_H\n#include \"lvgl.h\"\nvoid create_ui(lv_obj_t* parent);\n#endif" > $(GENERATED_UI_HEADER)
$(TARGET_CNC_NATIVE): $(OBJECTS) $(LVGL_LIB) ex_cnc/cnc_app.o ex_cnc/cnc_main_native.o $(GENERATED_UI_OBJ)
	@echo "\n--- Delegating to CNC Example Makefile (Native) ---\n"
	# $(MAKE) -C ex_cnc all
	$(CC) $(CFLAGS) -o $(TARGET_CNC_NATIVE) $^ $(LIBS)
	$(TARGET_CNC_NATIVE)

ex_cnc/cnc_main_live.o: ex_cnc/cnc_main.c
	$(CC) $(CFLAGS) -DCNC_LIVE_RENDER_MODE -c $< -o $@

ex_cnc/cnc_main_native.o: ex_cnc/cnc_main.c $(GENERATED_UI_HEADER)
	$(CC) $(CFLAGS) -DCNC_STATIC_BUILD_MODE -c $< -o $@

clean:
	@rm -f $(OBJECTS) $(TARGET) $(DYNAMIC_LVGL_H) $(DYNAMIC_LVGL_C) $(DYNAMIC_LVGL_O)
	@# rm -rf $(LVGL_BUILD_DIR)
	@rm -f $(TARGET_CNC_NATIVE) $(TARGET_CNC_RENDERED) $(GENERATED_UI_OBJ)

