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
SOURCES = main.c api_spec.c ir.c registry.c generator.c codegen.c utils.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)

run: $(TARGET)
	./$(TARGET) ./api_spec.json ./ui.json

%.o: %.c *.h
	@echo "Compiling $< with CFLAGS=$(CFLAGS)"
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
