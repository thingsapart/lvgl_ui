#ifndef LVGL_RENDERER_H
#define LVGL_RENDERER_H

#include "ir.h"
#include "api_spec.h"
#include "registry.h"
#include "c_gen/lvgl_dispatch.h" // Include the canonical definition

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
 * @brief Reloads the UI by cleaning the panels, re-parsing the UI spec,
 * and re-rendering the IR. This is the core of the watch mode functionality.
 *
 * @param ui_spec_path Path to the UI specification file (JSON or YAML).
 * @param api_spec The parsed API specification.
 * @param preview_panel The LVGL object to render the main UI onto.
 * @param inspector_panel The LVGL object to render the inspector UI onto.
 */
void lvgl_renderer_reload_ui(const char* ui_spec_path, ApiSpec* api_spec, lv_obj_t* preview_panel, lv_obj_t* inspector_panel);

#endif // LVGL_RENDERER_H
