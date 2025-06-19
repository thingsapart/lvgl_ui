CC = gcc
CFLAGS = -Wall -g -std=c11 -I.
LIBS = -LcJSON -lm ./cJSON/libcjson.a

TARGET = lvgl_ui_generator
SOURCES = main.c api_spec.c ir.c registry.c generator.c codegen.c utils.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)

%.o: %.c *.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
