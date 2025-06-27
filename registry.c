#include "registry.h"
#include <stdlib.h> // For malloc, calloc, free, NULL
#include <string.h> // For strdup, strcmp
#include <stdio.h> // For perror, fprintf, stderr
#include "utils.h" // For render_abort
#include "debug_log.h"

// --- Implementation ---

Registry* registry_create() {
    Registry* reg = (Registry*)calloc(1, sizeof(Registry));
    if (!reg) {
        perror("Failed to allocate registry");
        return NULL;
    }
    return reg;
}

void registry_free(Registry* reg) {
    if (!reg) return;

    ComponentRegistryNode* current_comp = reg->components;
    while (current_comp) {
        ComponentRegistryNode* next_comp = current_comp->next;
        free(current_comp->name);
        // Note: component_root is a pointer to a node in the main cJSON doc,
        // so we do NOT free it here. It's freed when cJSON_Delete is called on the root.
        free(current_comp);
        current_comp = next_comp;
    }

    VarRegistryNode* current_var = reg->generated_vars;
    while (current_var) {
        VarRegistryNode* next_var = current_var->next;
        free(current_var->name);
        free(current_var->c_var_name);
        free(current_var->c_type);
        free(current_var);
        current_var = next_var;
    }

    PointerRegistryNode* current_ptr_node = reg->pointers;
    while (current_ptr_node) {
        PointerRegistryNode* next_ptr_node = current_ptr_node->next;
        free(current_ptr_node->id);
        free(current_ptr_node->json_type);
        free(current_ptr_node->c_type);
        free(current_ptr_node);
        current_ptr_node = next_ptr_node;
    }

    StringRegistryNode* current_str_node = reg->strings;
    while (current_str_node) {
        StringRegistryNode* next_str_node = current_str_node->next;
        free(current_str_node->value);
        free(current_str_node);
        current_str_node = next_str_node;
    }

    free(reg);
}

// --- Pointer Registration ---

void registry_add_pointer(Registry* reg, void* ptr, const char* id, const char* json_type, const char* c_type) {
    if (!reg || !id || !ptr) return;

    PointerRegistryNode* new_node = (PointerRegistryNode*)calloc(1, sizeof(PointerRegistryNode));
    if (!new_node) render_abort("Failed to allocate pointer registry node");

    new_node->id = strdup(id);
    new_node->json_type = strdup(json_type);
    new_node->c_type = strdup(c_type);
    new_node->ptr = ptr;
    new_node->next = reg->pointers;
    reg->pointers = new_node;
}

void* registry_get_pointer(const Registry* reg, const char* id, const char* type) {
    if (!reg || !id) return NULL;
    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        if (strcmp(node->id, id) == 0) {
            if (type && node->json_type && strcmp(node->json_type, type) == 0) {
                return node->ptr;
            } else if (!type) {
                return node->ptr;
            }
        }
    }
    return NULL;
}


// --- String Registration ---

const char* registry_add_str(Registry* reg, const char* value) {
    if (!reg || !value) return NULL;
    for (StringRegistryNode* node = reg->strings; node; node = node->next) {
        if (strcmp(node->value, value) == 0) return node->value;
    }

    StringRegistryNode* new_node = (StringRegistryNode*)malloc(sizeof(StringRegistryNode));
    if (!new_node) render_abort("Failed to allocate string registry node");

    new_node->value = strdup(value);
    new_node->next = reg->strings;
    reg->strings = new_node;
    return new_node->value;
}


void registry_add_component(Registry* reg, const char* name, const cJSON* component_root) {
    if (!reg || !name || !component_root) return;
    const char* key = (name[0] == '@') ? name + 1 : name;

    ComponentRegistryNode* new_node = (ComponentRegistryNode*)malloc(sizeof(ComponentRegistryNode));
    if (!new_node) render_abort("Failed to allocate component registry node");
    new_node->name = strdup(key);
    new_node->component_root = component_root;
    new_node->next = reg->components;
    reg->components = new_node;
}

const cJSON* registry_get_component(const Registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    const char* key = (name[0] == '@') ? name + 1 : name;

    for (ComponentRegistryNode* node = reg->components; node; node = node->next) {
        if (strcmp(node->name, key) == 0) return node->component_root;
    }
    return NULL;
}

void registry_add_generated_var(Registry* reg, const char* name, const char* c_var_name, const char* c_type) {
    if (!reg || !name || !c_var_name) return;

    VarRegistryNode* new_node = (VarRegistryNode*)malloc(sizeof(VarRegistryNode));
    if (!new_node) render_abort("Failed to allocate var registry node");
    new_node->name = strdup(name);
    new_node->c_var_name = strdup(c_var_name);
    new_node->c_type = strdup(c_type);

    new_node->next = reg->generated_vars;
    reg->generated_vars = new_node;
}

const char* registry_get_generated_var(const Registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    for (VarRegistryNode* node = reg->generated_vars; node; node = node->next) {
        if (strcmp(node->name, name) == 0) return node->c_var_name;
    }
    return NULL;
}

const char* registry_get_c_type_for_id(const Registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    // Check generated variables first
    for (VarRegistryNode* node = reg->generated_vars; node; node = node->next) {
        if (strcmp(node->name, name) == 0) return node->c_type;
    }
    // Check runtime pointers
    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        if (strcmp(node->id, name) == 0) return node->c_type;
    }
    return NULL; // Not found
}
