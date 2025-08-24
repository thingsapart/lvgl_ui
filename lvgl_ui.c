#include "lvgl_ui.h"

void lvgl_ui_init(void) {
    data_binding_init();
    obj_registry_init();
}

void lvgl_ui_deinit(void) {
    obj_registry_deinit();
    // data_binding doesn't have a separate deinit; its resources are freed
    // when the associated widgets are deleted, which triggers LV_EVENT_DELETE.
}
