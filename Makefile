CC = gcc
CFLAGS = -Wall -g -std=c11 -I. -I/usr/include/cjson -I./cJSON -D_GNU_SOURCE
LIBS = -lm # Removed ./cJSON/libcjson.a

TARGET = lvgl_ui_generator
SOURCES = main.c api_spec.c ir.c registry.c generator.c codegen.c utils.c cJSON/cJSON.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)

%.o: %.c *.h
	@echo "Compiling $< with CFLAGS=$(CFLAGS)"
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
