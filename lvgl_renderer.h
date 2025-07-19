#ifndef LVGL_RENDERER_H
#define LVGL_RENDERER_H

#include "ir.h"
#include "api_spec.h"
#include "registry.h"
#include "c_gen/lvgl_dispatch.h" // Include the canonical definition

// --- Configuration ---

/**
 * @brief When defined to 1, the live renderer will treat an unresolved registry reference
 * (e.g., a typo in an object ID like `@my_wodget`) as a fatal error for the
 * current render pass. It will stop rendering and print an error.
 * If not defined, it will print a warning and continue by substituting NULL,
 * which may lead to an LV_ASSERT and a server crash. For the VSCode server,
 * this should be defined to ensure graceful recovery.
 */
#ifndef RENDERER_ABORT_ON_UNRESOLVED_REFERENCE
#define RENDERER_ABORT_ON_UNRESOLVED_REFERENCE 1
#endif

// --- Main Backend Entry Point ---

/**
 * @brief Renders the given IR tree into a live LVGL UI.
 * This is the main entry point for the "lvgl_render" backend.
 * @param root The root of the IR tree to render.
 * @param api_spec The parsed API specification.
 * @param parent The LVGL object to render the UI onto.
 * @param registry The registry to use for storing object pointers and static data.
 *                 The caller is responsible for the lifecycle of this registry.
 */
void lvgl_render_backend(IRRoot* root, ApiSpec* api_spec, lv_obj_t* parent, Registry* registry);

/**
 * @brief Reloads the UI by cleaning the panels, re-parsing the UI spec from a file,
 * and re-rendering the IR.
 *
 * @param ui_spec_path Path to the UI specification file (JSON or YAML).
 * @param api_spec The parsed API specification.
 * @param preview_panel The LVGL object to render the main UI onto.
 * @param inspector_panel The LVGL object to render the inspector UI onto.
 */
void lvgl_renderer_reload_ui(const char* ui_spec_path, ApiSpec* api_spec, lv_obj_t* preview_panel, lv_obj_t* inspector_panel);

/**
 * @brief Reloads the UI by cleaning the panels, re-parsing the UI spec from a string,
 * and re-rendering the IR. This is the core of the watch mode functionality.
 *
 * @param ui_spec_string A string containing the UI specification (JSON or YAML).
 * @param api_spec The parsed API specification.
 * @param preview_panel The LVGL object to render the main UI onto.
 * @param inspector_panel The LVGL object to render the inspector UI onto.
 */
void lvgl_renderer_reload_ui_from_string(const char* ui_spec_string, ApiSpec* api_spec, lv_obj_t* preview_panel, lv_obj_t* inspector_panel);


#endif // LVGL_RENDERER_H
