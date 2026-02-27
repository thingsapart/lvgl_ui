#ifndef DEFERRED_LOADER_H
#define DEFERRED_LOADER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Function signature for a deferred child-creation function.
 *
 * The generated code emits one such function per widget that carries the
 * `deferred` YAML property.  The function creates all children of that widget
 * and installs their properties; `parent` is the widget whose children should
 * be created.
 */
typedef void (*deferred_create_fn_t)(lv_obj_t* parent);

/**
 * @brief Register a deferred child with its scroll-container parent.
 *
 * Call this once per deferred child, in creation order, before calling
 * deferred_loader_init() for the same parent.  The child's content is NOT
 * created here; it will be created on demand when the child becomes active.
 *
 * @param scroll_parent  The tabview / tileview (or other paged widget) that
 *                       owns the child.
 * @param child          The child panel / tile returned by lv_tabview_add_tab
 *                       etc.
 * @param create_fn      The generated static function that populates the child.
 */
void deferred_loader_register(lv_obj_t* scroll_parent,
                               lv_obj_t* child,
                               deferred_create_fn_t create_fn);

/**
 * @brief Finalise deferred loading for a parent widget.
 *
 * Installs LV_EVENT_VALUE_CHANGED and LV_EVENT_DELETE handlers on the parent
 * so that children are created/destroyed as the active page changes.
 * Marks the parent dirty so that the first deferred_loader_task_handler() call
 * populates the currently-active child.
 *
 * Must be called after all deferred_loader_register() calls for the same
 * parent.
 *
 * @param scroll_parent  The same widget passed to deferred_loader_register().
 */
void deferred_loader_init(lv_obj_t* scroll_parent);

/**
 * @brief Process all pending deferred-load work.
 *
 * Must be called by the application once per main loop iteration, immediately
 * after lv_task_handler().  Running here (outside LVGL's draw/event cycle and
 * outside any ISR) is safe for object creation, lv_malloc, and FreeRTOS
 * primitives.
 *
 * Typical usage:
 * @code
 *   lv_task_handler();
 *   lvgl_ui_task_handler(); // or call deferred_loader_task_handler() directly
 * @endcode
 */
void deferred_loader_task_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* DEFERRED_LOADER_H */
