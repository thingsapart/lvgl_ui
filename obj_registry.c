#include "obj_registry.h"
#include "lvgl_ui_tils.h" // For print_warning
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef DYNAMIC_LVGL_MAX_OBJECTS
#define DYNAMIC_LVGL_MAX_OBJECTS 1024
#endif

typedef struct {
    char* id;
    void* obj;
} ObjectEntry;

static ObjectEntry obj_registry[DYNAMIC_LVGL_MAX_OBJECTS];
static int obj_registry_count = 0;

void obj_registry_init(void) {
    obj_registry_count = 0;
    memset(obj_registry, 0, sizeof(obj_registry));
}

char* obj_registry_add_str(const char *s) {
    if (!s) return NULL;
    if (obj_registry_count >= DYNAMIC_LVGL_MAX_OBJECTS) {
        print_warning("Cannot add string to registry: registry full");
        return (char*)s;
    }
    size_t slen = strlen(s);
    char* id_buf = malloc(slen + 6);
    if (!id_buf) return (char*)s;
    sprintf(id_buf, "str::%s", s);

    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, id_buf) == 0) {
            free(id_buf);
            return (char*)obj_registry[i].obj;
        }
    }

    obj_registry[obj_registry_count].id = id_buf;
    obj_registry[obj_registry_count].obj = strdup(s);
    return (char*)obj_registry[obj_registry_count++].obj;
}

void obj_registry_add(const char* id, void* obj) {
    if (!id) return;

    // First, check if the ID already exists to update it.
    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, id) == 0) {
            obj_registry[i].obj = obj;
            return;
        }
    }

    // If not found, add a new entry if there's space.
    if (obj_registry_count >= DYNAMIC_LVGL_MAX_OBJECTS) {
        print_warning("Cannot add object to registry: full or null ID");
        return;
    }

    obj_registry[obj_registry_count].id = strdup(id);
    obj_registry[obj_registry_count].obj = obj;
    obj_registry_count++;
}

void* obj_registry_get(const char* id) {
    if (!id) return NULL;
    if (strcmp(id, "SCREEN_ACTIVE") == 0) return (void*)lv_screen_active();
    if (strcmp(id, "NULL") == 0) return NULL;
    if (strcmp(id, "lv_font_default") == 0 || strcmp(id, "@lv_font_default") == 0) return (void*)LV_FONT_DEFAULT;

    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, id) == 0) {
            return obj_registry[i].obj;
        }
    }
    const char* key = (id[0] == '@') ? id + 1 : id;
    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, key) == 0) {
            return obj_registry[i].obj;
        }
    }

    print_warning("Object with ID '%s' not found in registry.", id);
    return NULL;
}

void obj_registry_deinit(void) {
    for (int i = 0; i < obj_registry_count; i++) {
        if(obj_registry[i].id) free(obj_registry[i].id);
        // Free strings that were specifically allocated by the registry
        if(obj_registry[i].obj && strncmp(obj_registry[i].id, "str::", 5) == 0) {
            free(obj_registry[i].obj);
        }
    }
    obj_registry_init();
}
