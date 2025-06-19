#include "registry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h> // For NULL if not in stdlib

// --- Internal Data Structures ---

typedef struct ComponentRegistryNode {
    char* name;                 // Key (e.g., "comp_name" from "@comp_name")
    const cJSON* component_root; // Value (pointer to the cJSON object)
    struct ComponentRegistryNode* next;
} ComponentRegistryNode;

typedef struct VarRegistryNode {
    char* name;        // Key (e.g., "style_id" from "@style_id")
    char* c_var_name;  // Value (e.g., "style_0" or "button_1")
    struct VarRegistryNode* next;
} VarRegistryNode;

struct Registry {
    ComponentRegistryNode* components;
    VarRegistryNode* generated_vars;
};

// --- Implementation ---

Registry* registry_create() {
    Registry* reg = (Registry*)calloc(1, sizeof(Registry));
    if (!reg) {
        perror("Failed to allocate registry");
        return NULL;
    }
    reg->components = NULL;
    reg->generated_vars = NULL;
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

    free(reg);
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
