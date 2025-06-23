#include "registry.h"
#include <stdlib.h> // For malloc, calloc, free, NULL
#include <string.h> // For strdup, strcmp
#include <stdio.h> // For perror, fprintf, stderr

// --- Internal Data Structures ---
// The definitions for ComponentRegistryNode, VarRegistryNode, and Registry struct
// are now in registry.h. This file (registry.c) will use those definitions.

// --- Implementation ---

Registry* registry_create() {
    Registry* reg = (Registry*)calloc(1, sizeof(Registry));
    if (!reg) {
        perror("Failed to allocate registry");
        return NULL;
    }
    reg->components = NULL;
    reg->generated_vars = NULL;
    reg->pointers = NULL; // Initialize new member
    reg->strings = NULL;  // Initialize new member
    return reg;
}

void registry_free(Registry* reg) {
    if (!reg) return;

    // Free components
    ComponentRegistryNode* current_comp = reg->components;
    while (current_comp) {
        ComponentRegistryNode* next_comp = current_comp->next;
        free(current_comp->name);
        // Note: We do NOT free current_comp->component_root here.
        // The cJSON objects are owned by the main cJSON document parsed in main.c
        // and will be freed when cJSON_Delete is called on the root UI spec JSON.
        free(current_comp);
        current_comp = next_comp;
    }

    // Free generated variables
    VarRegistryNode* current_var = reg->generated_vars;
    while (current_var) {
        VarRegistryNode* next_var = current_var->next;
        free(current_var->name);
        free(current_var->c_var_name);
        free(current_var);
        current_var = next_var;
    }

    // Free registered pointers
    PointerRegistryNode* current_ptr_node = reg->pointers;
    while (current_ptr_node) {
        PointerRegistryNode* next_ptr_node = current_ptr_node->next;
        free(current_ptr_node->id);
        if (current_ptr_node->json_type) { // Updated field name
            free(current_ptr_node->json_type);
        }
        // DO NOT free current_ptr_node->ptr - registry does not own it
        free(current_ptr_node);
        current_ptr_node = next_ptr_node;
    }

    // Free registered strings
    StringRegistryNode* current_str_node = reg->strings;
    while (current_str_node) {
        StringRegistryNode* next_str_node = current_str_node->next;
        free(current_str_node->value); // Strings were strdup'd
        free(current_str_node);
        current_str_node = next_str_node;
    }

    free(reg);
}

// --- Pointer Registration ---

void registry_add_pointer(Registry* reg, void* ptr, const char* id, const char* type) {
    if (!reg || !id || !ptr) {
        fprintf(stderr, "Error: registry_add_pointer: reg, id, and ptr must not be NULL\n");
        return;
    }

    PointerRegistryNode* new_node = (PointerRegistryNode*)malloc(sizeof(PointerRegistryNode));
    if (!new_node) {
        perror("Failed to allocate pointer registry node");
        return;
    }

    new_node->id = strdup(id);
    if (!new_node->id) {
        perror("Failed to duplicate id string for pointer registry");
        free(new_node);
        return;
    }

    if (type) {
        new_node->json_type = strdup(type);
        if (!new_node->json_type) {
            perror("Failed to duplicate type string for pointer registry");
            free(new_node->id);
            free(new_node);
            return;
        }
    } else {
        new_node->json_type = NULL;
    }

    new_node->ptr = ptr;
    new_node->next = reg->pointers;
    reg->pointers = new_node;
}

void* registry_get_pointer(const Registry* reg, const char* id, const char* type) {
    if (!reg || !id) return NULL;

    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        if (strcmp(node->id, id) == 0) {
            if (type) { // Type is specified, so it must match
                if (node->json_type && strcmp(node->json_type, type) == 0) {
                    return node->ptr;
                }
            } else { // Type is not specified, first ID match is sufficient
                return node->ptr;
            }
        }
    }
    return NULL; // Not found
}

const char* registry_get_json_type_for_id(const Registry* reg, const char* id) {
    if (!reg || !id) return NULL;

    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        if (node->id && strcmp(node->id, id) == 0) {
            return node->json_type; // This 'json_type' field stores the json_type
        }
    }
    return NULL; // Not found or no type stored
}


// --- String Registration ---

const char* registry_add_str(Registry* reg, const char* value) {
    if (!reg || !value) {
         fprintf(stderr, "Error: registry_add_str: reg and value must not be NULL\n");
        return NULL; // Or consider returning a static error string
    }

    // Check if string already exists
    for (StringRegistryNode* node = reg->strings; node; node = node->next) {
        if (strcmp(node->value, value) == 0) {
            return node->value; // Return existing string
        }
    }

    // String not found, create and add new node
    StringRegistryNode* new_node = (StringRegistryNode*)malloc(sizeof(StringRegistryNode));
    if (!new_node) {
        perror("Failed to allocate string registry node");
        return NULL; // Or consider returning a static error string
    }

    new_node->value = strdup(value);
    if (!new_node->value) {
        perror("Failed to duplicate string for string registry");
        free(new_node);
        return NULL; // Or consider returning a static error string
    }

    new_node->next = reg->strings;
    reg->strings = new_node;
    return new_node->value; // Return newly duplicated string
}


void registry_add_component(Registry* reg, const char* name, const cJSON* component_root) {
    if (!reg || !name || !component_root) return;

    // Check if component already exists (optional, could update or error)
    for (ComponentRegistryNode* node = reg->components; node; node = node->next) {
        if (strcmp(node->name, name) == 0) {
            fprintf(stderr, "Warning: Component '%s' already registered. Overwriting.\n", name);
            node->component_root = component_root; // Update existing
            return;
        }
    }

    ComponentRegistryNode* new_node = (ComponentRegistryNode*)malloc(sizeof(ComponentRegistryNode));
    if (!new_node) {
        perror("Failed to allocate component registry node");
        return;
    }
    new_node->name = strdup(name);
    if (!new_node->name) {
        perror("Failed to duplicate component name string");
        free(new_node);
        return;
    }
    new_node->component_root = component_root; // Store direct pointer
    new_node->next = reg->components;
    reg->components = new_node;
}

const cJSON* registry_get_component(const Registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    for (ComponentRegistryNode* node = reg->components; node; node = node->next) {
        if (strcmp(node->name, name) == 0) {
            return node->component_root;
        }
    }
    fprintf(stderr, "Warning: Component '%s' not found in registry.\n", name);
    return NULL;
}

void registry_add_generated_var(Registry* reg, const char* name, const char* c_var_name) {
    if (!reg || !name || !c_var_name) return;

    // Check if var already exists (optional)
    for (VarRegistryNode* node = reg->generated_vars; node; node = node->next) {
        if (strcmp(node->name, name) == 0) {
            fprintf(stderr, "Warning: Generated variable '%s' already registered. Updating C name from '%s' to '%s'.\n", name, node->c_var_name, c_var_name);
            free(node->c_var_name);
            node->c_var_name = strdup(c_var_name);
            if (!node->c_var_name) {
                perror("Failed to duplicate C variable name string for update");
            }
            return;
        }
    }

    VarRegistryNode* new_node = (VarRegistryNode*)malloc(sizeof(VarRegistryNode));
    if (!new_node) {
        perror("Failed to allocate var registry node");
        return;
    }
    new_node->name = strdup(name);
    new_node->c_var_name = strdup(c_var_name);

    if (!new_node->name || !new_node->c_var_name) {
        perror("Failed to duplicate string for var registry node");
        free(new_node->name);
        free(new_node->c_var_name);
        free(new_node);
        return;
    }

    new_node->next = reg->generated_vars;
    reg->generated_vars = new_node;
}

const char* registry_get_generated_var(const Registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    for (VarRegistryNode* node = reg->generated_vars; node; node = node->next) {
        if (strcmp(node->name, name) == 0) {
            return node->c_var_name;
        }
    }
    // It's not an error if a @variable is not in this *generated* var registry,
    // as it might be a C-registered pointer (e.g. @font_xyz).
    // The generator will handle this by outputting the name directly.
    // However, for styles or named widgets defined *within* the JSON, they *should* be found.
    // Consider if a warning is appropriate here or if the caller should handle "not found".
    // For now, no warning, consistent with current generator.c logic.
    return NULL;
}
