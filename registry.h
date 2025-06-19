#ifndef REGISTRY_H
#define REGISTRY_H

#include <cJSON.h> // Changed from <cJSON/cJSON.h>

// --- Opaque pointer for the registry ---
typedef struct Registry Registry;

// --- Lifecycle ---
Registry* registry_create();
void registry_free(Registry* reg);

// --- Component Management ---
// Components are top-level objects from the "components" section of the UI spec.
// They are identified by a name (e.g., "my_custom_button").
// The `component_root` is a pointer to the cJSON object defining the component.
// This cJSON object is owned by the main parsed document and should NOT be freed by the registry.
void registry_add_component(Registry* reg, const char* name, const cJSON* component_root);
const cJSON* registry_get_component(const Registry* reg, const char* name);

// --- Generated Variable Name Management ---
// Manages mappings from a UI-spec name (e.g., "style_main_button", "widget_login_button")
// to a generated C variable name (e.g., "style_0", "button_1").
// This is used to ensure that if a style or widget is referenced by its name multiple times,
// we re-use the same generated C variable.
// Both `name` and `c_var_name` are duplicated by the registry and freed by `registry_free`.
void registry_add_generated_var(Registry* reg, const char* name, const char* c_var_name);
const char* registry_get_generated_var(const Registry* reg, const char* name);

#endif // REGISTRY_H
