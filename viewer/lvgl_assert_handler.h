#ifndef LVGL_ASSERT_HANDLER_H
#define LVGL_ASSERT_HANDLER_H

/**
 * @file lvgl_assert_handler.h
 * @brief This header is included by LVGL via lv_conf.h to bridge LVGL's
 * assert mechanism with the LVGLode server's error reporting.
 */

// Declare the global function pointer for the LVGL assert handler.
// This pointer will be defined and set in main_vsc.c.
// It allows the main application to register a callback that LVGL's
// assert macro can invoke.
extern void (*g_lvgl_assert_abort_cb)(const char *);

#endif // LVGL_ASSERT_HANDLER_H
