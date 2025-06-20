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
        if (current_ptr_node->type) {
            free(current_ptr_node->type);
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
    if (!reg || !id ) { // ptr can be NULL (used by generator for type registration)
        fprintf(stderr, "Error: registry_add_pointer: reg and id must not be NULL\n");
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
        new_node->type = strdup(type);
        if (!new_node->type) {
            perror("Failed to duplicate type string for pointer registry");
            free(new_node->id);
            free(new_node);
            return;
        }
    } else {
        new_node->type = NULL;
    }

    new_node->ptr = ptr;
    new_node->next = reg->pointers;
    reg->pointers = new_node;
}

void* registry_get_pointer(const Registry* reg, const char* id, const char* expected_type) {
    if (!reg || !id) return NULL;

    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        if (strcmp(node->id, id) == 0) {
            if (expected_type != NULL && node->type != NULL && strcmp(node->type, expected_type) != 0) {
                fprintf(stderr, "Warning: Registry type mismatch for ID '%s'. Expected '%s', but found '%s'.\n", id, expected_type, node->type);
            }
            return node->ptr;
        }
    }
    return NULL;
}

void *registry_get_pointer_by_id(Registry *reg, const char *id, const char **type_out) {
  if (!reg || !id) {
    if (type_out) *type_out = NULL;
    return NULL;
  }
  PointerRegistryNode *current = reg->pointers;
  while (current != NULL) {
    if (strcmp(current->id, id) == 0) {
      if (type_out != NULL) {
        *type_out = current->type;
      }
      return current->ptr;
    }
    current = current->next;
  }
  if (type_out != NULL) {
    *type_out = NULL;
  }
  return NULL;
}

const char *registry_get_type_by_id(const Registry *reg, const char *id) {
  if (!reg || !id) return NULL;
  PointerRegistryNode *current = reg->pointers;
  while (current != NULL) {
    if (strcmp(current->id, id) == 0) {
      return current->type;
    }
    current = current->next;
  }
  return NULL;
}

// --- String Registration ---

const char* registry_add_str(Registry* reg, const char* value) {
    if (!reg || !value) {
         fprintf(stderr, "Error: registry_add_str: reg and value must not be NULL\n");
        return NULL;
    }

    for (StringRegistryNode* node = reg->strings; node; node = node->next) {
        if (strcmp(node->value, value) == 0) {
            return node->value;
        }
    }

    StringRegistryNode* new_node = (StringRegistryNode*)malloc(sizeof(StringRegistryNode));
    if (!new_node) {
        perror("Failed to allocate string registry node");
        return NULL;
    }

    new_node->value = strdup(value);
    if (!new_node->value) {
        perror("Failed to duplicate string for string registry");
        free(new_node);
        return NULL;
    }

    new_node->next = reg->strings;
    reg->strings = new_node;
    return new_node->value;
}


void registry_add_component(Registry* reg, const char* name, const cJSON* component_root) {
    if (!reg || !name || !component_root) return;

    for (ComponentRegistryNode* node = reg->components; node; node = node->next) {
        if (strcmp(node->name, name) == 0) {
            // fprintf(stderr, "Warning: Component '%s' already registered. Overwriting.\n", name);
            node->component_root = component_root;
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
    new_node->component_root = component_root;
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
    // fprintf(stderr, "Warning: Component '%s' not found in registry.\n", name); // Can be too noisy
    return NULL;
}

void registry_add_generated_var(Registry* reg, const char* name, const char* c_var_name) {
    if (!reg || !name || !c_var_name) return;

    for (VarRegistryNode* node = reg->generated_vars; node; node = node->next) {
        if (strcmp(node->name, name) == 0) {
            // If name (original ID) already exists, update its C variable name if different.
            if (strcmp(node->c_var_name, c_var_name) != 0) {
                // fprintf(stderr, "Warning: Generated variable for ID '%s' already registered. Updating C name from '%s' to '%s'.\n", name, node->c_var_name, c_var_name);
                free(node->c_var_name);
                node->c_var_name = strdup(c_var_name);
                if (!node->c_var_name) {
                    perror("Failed to duplicate C variable name string for update");
                }
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
    return NULL;
}

// Gets original UI name/ID by its generated C variable name
const char* registry_get_id_by_gen_var(const Registry* reg, const char* c_var_name) {
    if (!reg || !c_var_name) return NULL;
    for (VarRegistryNode* node = reg->generated_vars; node; node = node->next) {
        if (strcmp(node->c_var_name, c_var_name) == 0) {
            return node->name; // This 'name' is the original ID.
        }
    }
    return NULL;
}
