#ifndef LVGL_RENDERER_H
#define LVGL_RENDERER_H

#include "ir.h"
#include "api_spec.h"
#include "lvgl.h"
#include "registry.h"

/**
 * @brief The main function for the dynamic LVGL rendering backend.
 *
 * This function traverses the IR tree and executes it at runtime, creating a live
 * LVGL UI. It uses a dynamically-dispatched function call mechanism to
 * interact with the LVGL library. This backend is intended for applications
 * that need to load and render UI definitions on the fly.
 *
 * @param root The root of the IR tree to render.
 * @param api_spec The parsed API specification, needed by the dispatcher.
 * @param parent The top-level lv_obj_t* to which the new UI will be parented.
 * @param registry A pre-initialized registry to store created objects and arrays.
 *                 The caller is responsible for its lifecycle.
 */
void lvgl_render_backend(IRRoot* root, ApiSpec* api_spec, lv_obj_t* parent, Registry* registry);

#endif // LVGL_RENDERER_H
