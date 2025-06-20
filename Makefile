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
SOURCES = main.c api_spec.c ir.c registry.c generator.c codegen.c utils.c cJSON/cJSON.c
OBJECTS = $(SOURCES:.c=.o)

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
	rm -f $(OBJECTS) $(TARGET) $(TEST_REGISTRY_BIN) $(TEST_GENERATOR_REGISTRY_STRINGS_BIN) $(TEST_GENERATOR_REGISTRY_POINTERS_BIN)
