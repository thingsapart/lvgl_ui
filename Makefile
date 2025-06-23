CC = gcc

ifneq ("$(wildcard ./cJSON/libcjson.a)","")
    LIB_CJSON = ./cJSON/libcjson.a
		INC_CJSON = -I./cJSON
else
    LIBCJSON = -lcjson
		INC_CJSON = -I/usr/include/cjson
endif

CFLAGS = -Wall -g -std=c11 -I. $(INC_CJSON) -I./cJSON -D_GNU_SOURCE
LIBS = $(LIB_CJSON) -lm

TARGET = lvgl_ui_generator

# Python script for generating dynamic_lvgl.c and .h
API_SPEC_GENERATOR_PY = ./generate_dynamic_lvgl_dispatch.py
API_SPEC_JSON = ./api_spec.json # Input for the generator script
DYNAMIC_LVGL_H = ./dynamic_lvgl.h
DYNAMIC_LVGL_C = ./dynamic_lvgl.c
DYNAMIC_LVGL_O = $(DYNAMIC_LVGL_C:.c=.o)

# Options for the python script
CONSOLIDATION_MODE ?= typesafe # Default to typesafe, can be overridden: make CONSOLIDATION_MODE=aggressive
# To enable specific input methods for dynamic_lvgl:
# make DYNAMIC_LVGL_CFLAGS="-DENABLE_CJSON_INPUTS -DENABLE_IR_INPUTS"
DYNAMIC_LVGL_CFLAGS ?= -DENABLE_CJSON_INPUTS # Default to only CJSON inputs

# Add DYNAMIC_LVGL_CFLAGS to general CFLAGS
CFLAGS += $(DYNAMIC_LVGL_CFLAGS)

SOURCES = main.c api_spec.c ir.c registry.c generator.c codegen.c utils.c cJSON/cJSON.c $(DYNAMIC_LVGL_C)
OBJECTS = $(SOURCES:.c=.o)

# Rule to generate dynamic_lvgl.h and dynamic_lvgl.c
# These files depend on the Python script and the api_spec.json
$(DYNAMIC_LVGL_H) $(DYNAMIC_LVGL_C): $(API_SPEC_GENERATOR_PY) $(API_SPEC_JSON)
	@echo "Generating $(DYNAMIC_LVGL_H) and $(DYNAMIC_LVGL_C) with consolidation mode: $(CONSOLIDATION_MODE)"
	python3 $(API_SPEC_GENERATOR_PY) $(API_SPEC_JSON) \
		--consolidation-mode $(CONSOLIDATION_MODE) \
		--header-out $(DYNAMIC_LVGL_H) \
		--source-out $(DYNAMIC_LVGL_C)

# Ensure generated files are created before objects that depend on them are compiled.
# Specifically, main.o or any other .o that might include dynamic_lvgl.h
# and the dynamic_lvgl.o itself.
$(DYNAMIC_LVGL_O): $(DYNAMIC_LVGL_C) $(DYNAMIC_LVGL_H)
main.o: $(DYNAMIC_LVGL_H)
# Add other .o files that include dynamic_lvgl.h as prerequisites if needed

# Test sources
TEST_REGISTRY_SRC = tests/test_registry.c registry.c utils.c cJSON/cJSON.c
TEST_REGISTRY_BIN = test_registry

# String registry test (with bypass)
TEST_GENERATOR_REGISTRY_STRINGS_SRC = tests/test_generator_registry_strings.c generator.c registry.c api_spec.c ir.c utils.c cJSON/cJSON.c codegen.c
TEST_GENERATOR_REGISTRY_STRINGS_BIN = test_generator_registry_strings

# Pointer registry test (no bypass, minimal spec)
TEST_GENERATOR_REGISTRY_POINTERS_SRC = tests/test_generator_registry_pointers.c generator.c registry.c api_spec.c ir.c utils.c cJSON/cJSON.c codegen.c
TEST_GENERATOR_REGISTRY_POINTERS_BIN = test_generator_registry_pointers

.PHONY: all clean test run_test_registry run_test_generator_registry_strings run_test_generator_registry_pointers

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)

run: $(TARGET)
	./$(TARGET) ./api_spec.json ./ui.json

%.o: %.c
	@echo "Compiling $< with CFLAGS=$(CFLAGS)"
	$(CC) $(CFLAGS) -c $< -o $@

# --- Tests ---
$(TEST_REGISTRY_BIN): $(TEST_REGISTRY_SRC) registry.h utils.h
	$(CC) $(CFLAGS) -o $(TEST_REGISTRY_BIN) $(TEST_REGISTRY_SRC) $(LIBS)

run_test_registry: $(TEST_REGISTRY_BIN)
	./$(TEST_REGISTRY_BIN)

# String registry test uses bypass
$(TEST_GENERATOR_REGISTRY_STRINGS_BIN): $(TEST_GENERATOR_REGISTRY_STRINGS_SRC) generator.h registry.h api_spec.h ir.h utils.h
	$(CC) $(CFLAGS) -DGENERATOR_REGISTRY_TEST_BYPASS_APISPEC_FIND_FOR_STRINGS -o $(TEST_GENERATOR_REGISTRY_STRINGS_BIN) $(TEST_GENERATOR_REGISTRY_STRINGS_SRC) $(LIBS)

run_test_generator_registry_strings: $(TEST_GENERATOR_REGISTRY_STRINGS_BIN)
	./$(TEST_GENERATOR_REGISTRY_STRINGS_BIN)

# Pointer registry test uses normal generator path (no bypass)
$(TEST_GENERATOR_REGISTRY_POINTERS_BIN): $(TEST_GENERATOR_REGISTRY_POINTERS_SRC) generator.h registry.h api_spec.h ir.h utils.h
	$(CC) $(CFLAGS) -o $(TEST_GENERATOR_REGISTRY_POINTERS_BIN) $(TEST_GENERATOR_REGISTRY_POINTERS_SRC) $(LIBS)

run_test_generator_registry_pointers: $(TEST_GENERATOR_REGISTRY_POINTERS_BIN)
	./$(TEST_GENERATOR_REGISTRY_POINTERS_BIN)

test: run_test_registry run_test_generator_registry_strings run_test_generator_registry_pointers

clean:
	rm -f $(OBJECTS) $(TARGET) $(TEST_REGISTRY_BIN) $(TEST_GENERATOR_REGISTRY_STRINGS_BIN) $(TEST_GENERATOR_REGISTRY_POINTERS_BIN) $(DYNAMIC_LVGL_H) $(DYNAMIC_LVGL_C) $(DYNAMIC_LVGL_O)
