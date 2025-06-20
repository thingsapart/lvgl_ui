#ifndef REGISTRY_H
#define REGISTRY_H

#include <cJSON/cJSON.h>

// --- Data structures for pointer and string registration ---
typedef struct PointerRegistryNode {
    char* id;       // ID of the object (e.g., "my_button")
    char* type;     // C-type of the object (e.g., "lv_obj_t*", "lv_style_t")
    void* ptr;      // Actual pointer to the object in memory (mostly for runtime use, NULL during codegen)
    struct PointerRegistryNode* next;
} PointerRegistryNode;

typedef struct StringRegistryNode {
    char* value;    // The registered string literal
    struct StringRegistryNode* next;
} StringRegistryNode;

typedef struct ComponentRegistryNode {
    char* name;                 // Key (e.g., "comp_name" from "@comp_name")
    const cJSON* component_root; // Value (pointer to the cJSON object, owned by main JSON doc)
    struct ComponentRegistryNode* next;
} ComponentRegistryNode;

// Maps a name from UI spec (which could be an ID or a "named" field value)
// to a generated C variable name.
typedef struct VarRegistryNode {
    char* name;        // Key (e.g., "my_button_id" or "my_label_name")
    char* c_var_name;  // Value (e.g., "button_1", "label_0")
    // Note: The C-type of c_var_name is found via its original 'name' (ID) in PointerRegistryNode
    struct VarRegistryNode* next;
} VarRegistryNode;


// --- Opaque pointer for the registry ---
// typedef struct Registry Registry; // Not needed if struct is defined below

// --- Registry struct ---
// This is the full definition of the Registry.
struct Registry {
    ComponentRegistryNode* components;   // For component definitions
    VarRegistryNode* generated_vars;     // For mapping UI names/IDs to C variable names
    PointerRegistryNode* pointers;       // For mapping IDs to C-types and runtime pointers
    StringRegistryNode* strings;        // For global string literals
};
typedef struct Registry Registry;


// --- Lifecycle ---
Registry* registry_create();
void registry_free(Registry* reg);

// --- Component Management ---
void registry_add_component(Registry* reg, const char* name, const cJSON* component_root);
const cJSON* registry_get_component(const Registry* reg, const char* name);

// --- Generated Variable Name Management ---
void registry_add_generated_var(Registry* reg, const char* name, const char* c_var_name);
const char* registry_get_generated_var(const Registry* reg, const char* name); // Gets C-var by its original UI name/ID
const char* registry_get_id_by_gen_var(const Registry* reg, const char* c_var_name); // Gets original UI name/ID by C-var

// --- Pointer Registry (ID -> Type & Ptr) ---
void registry_add_pointer(Registry* reg, void *ptr, const char *id, const char *type);
void *registry_get_pointer_by_id(Registry *reg, const char *id, const char **type_out); // Gets ptr and C-type by ID
const char *registry_get_type_by_id(const Registry *reg, const char *id); // Gets C-type by ID
void *registry_get_pointer(const Registry* reg, const char *id, const char *expected_type); // Gets ptr by ID, warns on C-type mismatch

// --- String Registry ---
const char *registry_add_str(Registry* reg, const char *value);

#endif // REGISTRY_H
