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

#endif // LVGL_RENDERER_H
