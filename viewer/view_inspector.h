#ifndef VIEW_INSPECTOR_H
#define VIEW_INSPECTOR_H

#include "ir.h"
#include "api_spec.h"
#include "lvgl.h"

/**
 * @brief Initializes the Inspector UI within a given parent container.
 *
 * This function creates the two-pane layout for the tree view and property
 * inspector, and builds the internal tree representation from the IR.
 *
 * @param parent The LVGL object to create the inspector inside.
 * @param ir_root The root of the application's Intermediate Representation.
 * @param api_spec The parsed API specification, needed for context.
 */
void view_inspector_init(lv_obj_t* parent, IRRoot* ir_root, ApiSpec* api_spec);

/**
 * @brief Associates a live LVGL object pointer with its corresponding IR node.
 *
 * The lvgl_renderer calls this function after it creates a widget or style,
 * allowing the inspector to link the static IR definition to the live object.
 *
 * @param ir_node The IR node representing the object.
 * @param lv_ptr A pointer to the live lv_obj_t or lv_style_t.
 */
void view_inspector_set_object_pointer(IRNode* ir_node, void* lv_ptr);

#endif // VIEW_INSPECTOR_H
