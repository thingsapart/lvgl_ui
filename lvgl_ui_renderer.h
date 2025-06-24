#ifndef LVGL_UI_RENDERER_H
#define LVGL_UI_RENDERER_H

#include "ir.h"      // For IRStmtBlock
#include "lvgl.h"    // For lv_obj_t

// Forward declaration
struct ApiSpec;

/**
 * @brief Renders an LVGL UI based on an Intermediate Representation (IR) block.
 *
 * Iterates through the IR statements and executes them as LVGL calls
 * using dynamic dispatch. This function assumes lv_init() and display/input driver
 * initialization have already been performed.
 *
 * @param ir_block The block of IR statements representing the UI.
 * @param parent_screen The parent LVGL object to render the UI onto (usually lv_scr_act()).
 * @param api_spec Parsed API specification, used for method call resolution.
 */
void render_lvgl_ui_from_ir(IRStmtBlock* ir_block, lv_obj_t* parent_screen, struct ApiSpec* api_spec); // Use struct ApiSpec* for forward decl.

#endif // LVGL_UI_RENDERER_H
