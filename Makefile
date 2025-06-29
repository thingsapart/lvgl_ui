CC = gcc

ifneq ("$(wildcard ./cJSON/libcjson.a)","")
    LIB_CJSON = ./cJSON/libcjson.a
		INC_CJSON = -I./cJSON
else
    LIBCJSON = -lcjson
		INC_CJSON = -I/usr/include/cjson
endif

# Define the LVGL directory relative to this script
LVGL_DIR = $(PWD)/lvgl
LVGL_BUILD_DIR = $(LVGL_DIR)/build
LV_CONF_PATH = $(LVGL_DIR)/../viewer/lv_conf.h

# Define the path to the LVGL static library
LVGL_LIB = $(LVGL_BUILD_DIR)/lib/liblvgl.a

SDL_LIBS = `pkg-config sdl2 -libs`
SDL_CFLAGS = `pkg-config sdl2 -cflags`

LVGL_INC = -I./lvgl/src

CFLAGS = -Wall -g -std=c11 -I. $(INC_CJSON) -I./cJSON -D_GNU_SOURCE -I./lvgl -DLV_CONF_PATH='"$(LV_CONF_PATH)"' -DLV_BUILD_CONF_PATH='"$(LV_CONF_PATH)"' $(SDL_CFLAGS) $(LVGL_INC)
LIBS = $(LIB_CJSON) -lm $(LVGL_LIB) $(SDL_LIBS)

TARGET = lvgl_ui_generator

# Python script for generating lvgl_dispatch.c and .h
API_SPEC_GENERATOR_PY = ./generate_dynamic_lvgl_dispatch.py
API_SPEC_JSON = ./data/lv_def.json # Input for the generator script
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

# Removed viewer/sdl_viewer.c for now to bypass SDL dependency for core generator testing
SOURCES = main.c api_spec.c ir.c registry.c generator.c ir_printer.c ir_debug_printer.c c_code_printer.c utils.c debug_log.c cJSON/cJSON.c $(DYNAMIC_LVGL_C) viewer/sdl_viewer.c lvgl_renderer.c yaml_parser.c
OBJECTS = $(SOURCES:.c=.o)

# Main target rule now depends on the LVGL library
$(TARGET): $(OBJECTS) $(LVGL_LIB)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)

# Rule to build the LVGL static library if it doesn't exist.
# This rule is triggered because $(LVGL_LIB) is a prerequisite for $(TARGET).
$(LVGL_LIB):
	@echo "LVGL library not found or out of date. Building it..."
	./build_lvgl.sh

# Rule to generate dynamic_lvgl.h and dynamic_lvgl.c
# These files depend on the Python script and the api_spec.json
$(DYNAMIC_LVGL_H) $(DYNAMIC_LVGL_C): $(API_SPEC_GENERATOR_PY) $(API_SPEC_JSON)
	@echo "Generating $(DYNAMIC_LVGL_H) and $(DYNAMIC_LVGL_C) with consolidation mode: $(CONSOLIDATION_MODE)"
	python3 $(API_SPEC_GENERATOR_PY) $(API_SPEC_JSON) \
		--header-out $(DYNAMIC_LVGL_H) \
		--source-out $(DYNAMIC_LVGL_C)
		#--consolidation-mode $(CONSOLIDATION_MODE) \

# Ensure generated files are created before objects that depend on them are compiled.
# Specifically, main.o or any other .o that might include dynamic_lvgl.h
# and the dynamic_lvgl.o itself.
$(DYNAMIC_LVGL_O): $(DYNAMIC_LVGL_C) $(DYNAMIC_LVGL_H)
main.o: $(DYNAMIC_LVGL_H)

.PHONY: all clean

all: $(TARGET)

run: $(TARGET)
	./$(TARGET) ./api_spec.json ./ui.json

%.o: %.c
	@echo "Compiling $< with CFLAGS=$(CFLAGS)"
	$(CC) $(CFLAGS) -c $< -o $@

# Updated clean rule to also remove the LVGL build directory
clean:
	rm -f $(OBJECTS) $(TARGET) $(TEST_REGISTRY_BIN) $(TEST_GENERATOR_REGISTRY_STRINGS_BIN) $(TEST_GENERATOR_REGISTRY_POINTERS_BIN) $(DYNAMIC_LVGL_H) $(DYNAMIC_LVGL_C) $(DYNAMIC_LVGL_O)
	# rm -rf $(LVGL_BUILD_DIR)
