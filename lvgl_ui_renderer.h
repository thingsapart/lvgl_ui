#ifndef LVGL_UI_RENDERER_H
#define LVGL_UI_RENDERER_H

#include "ir.h"
#include "lvgl.h"
#include "api_spec.h"

/**
 * @brief Renders an LVGL UI based on a high-level Intermediate Representation (IR).
 *
 * Traverses the IR tree, creating LVGL objects and setting their properties
 * using dynamic dispatch. This function assumes lv_init() and display/input driver
 * initialization have already been performed.
 *
 * @param ir_root The root of the IR tree representing the UI.
 * @param parent_screen The parent LVGL object to render the UI onto (usually lv_screen_active()).
 * @param api_spec Parsed API specification, used for method call resolution.
 * @param initial_context Optional cJSON object providing initial values for top-level context variables.
 */
void render_lvgl_ui_from_ir(IRRoot* ir_root, lv_obj_t* parent_screen, ApiSpec* api_spec, cJSON* initial_context);

#endif // LVGL_UI_RENDERER_H
