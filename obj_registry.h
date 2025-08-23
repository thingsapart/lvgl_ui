#ifndef OBJ_REGISTRY_H
#define OBJ_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h" // For lv_screen_active

/**
 * @brief A simple runtime registry to map string IDs to created LVGL object pointers,
 * allowing the C application to retrieve and interact with named UI elements.
 */

/**
 * @brief Initializes or resets the object registry.
 */
void obj_registry_init(void);

/**
 * @brief Adds an object pointer to the registry with a given ID.
 * If the ID already exists, its pointer will be updated. This also handles
 * special string-only registrations needed by the live previewer.
 *
 * @param id The string identifier for the object.
 * @param obj A pointer to the lv_obj_t or other object.
 */
void obj_registry_add(const char* id, void* obj);

/**
 * @brief Registers a string that needs to persist for the lifetime of the UI.
 * This is primarily for the live-preview renderer to manage strings that
 * might otherwise go out of scope. The static C-code generator does not need this.
 *
 * @param s The string to register.
 * @return A pointer to the persistent, registered string.
 */
char* obj_registry_add_str(const char *s);

/**
 * @brief Retrieves an object pointer from the registry by its ID.
 *
 * @param id The string identifier of the object to find.
 * @return A pointer to the object, or NULL if not found.
 */
void* obj_registry_get(const char* id);

/**
 * @brief Deinitializes the object registry, freeing all stored IDs and strings.
 */
void obj_registry_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // OBJ_REGISTRY_H
