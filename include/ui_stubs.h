#ifndef UI_STUBS_H_
#define UI_STUBS_H_


typedef enum {
    LV_CAM_POS_MODE_NONE = 0,
    LV_CAM_POS_MODE_POINT = 1,
    LV_CAM_POS_MODE_RECTANGLE = 2,
    LV_CAM_POS_MODE_CIRCLE = 3,
} lv_cam_pos_mode_t;

typedef enum {
    LV_CAM_STREAM_FIT_CONTAIN = 0,
    LV_CAM_STREAM_FIT_COVER = 1,
    LV_CAM_STREAM_FIT_FILL = 2,
    LV_CAM_STREAM_FIT_NONE = 3,
} lv_cam_stream_fit_t;

typedef enum {
    IO_CAT_INPUTS = 0,
    IO_CAT_OUTPUTS = 1,
    IO_CAT_SENSORS = 2,
    IO_CAT_ACTUATORS = 3,
    IO_CAT__COUNT = 4,
} io_category_t;

typedef enum {
    LV_GCVIEW_TOP = 0,
    LV_GCVIEW_FRONT = 1,
    LV_GCVIEW_RIGHT = 2,
    LV_GCVIEW_LEFT = 3,
    LV_GCVIEW_BACK = 4,
    LV_GCVIEW_ISOMETRIC = 5,
    LV_GCVIEW_COUNT = 6,
} lv_gcode_viewer_view_t;

typedef enum {
    LV_PROBING_WIZARD_MODE_RECTANGLE = 0,
    LV_PROBING_WIZARD_MODE_CIRCLE = 1,
    LV_PROBING_WIZARD_MODE_CORNER = 2,
} lv_probing_wizard_mode_t;

typedef enum {
    LV_PROBING_CORNER_NONE = 0,
    LV_PROBING_CORNER_FRONT_LEFT = 1,
    LV_PROBING_CORNER_FRONT_RIGHT = 2,
    LV_PROBING_CORNER_BACK_LEFT = 3,
    LV_PROBING_CORNER_BACK_RIGHT = 4,
} lv_probing_wizard_corner_t;

typedef enum {
    HIGHLIGHT_NONE = 0,
    HIGHLIGHT_CORNER_BL = 0,
    HIGHLIGHT_CORNER_BR = 0,
    HIGHLIGHT_CORNER_FL = 0,
    HIGHLIGHT_CORNER_FR = 0,
    HIGHLIGHT_PROBE_POINT_0 = 0,
    HIGHLIGHT_PROBE_POINT_1 = 0,
    HIGHLIGHT_PROBE_POINT_2 = 0,
    HIGHLIGHT_PROBE_POINT_3 = 0,
    HIGHLIGHT_OUTLINE = 0,
    HIGHLIGHT_CENTER = 0,
    HIGHLIGHT_Z_PROBE = 0,
} highlight_part_t;

typedef enum {
    ACTION_AWAIT_START = 0,
    ACTION_JOG_AND_CONFIRM = 1,
    ACTION_SELECT_CORNER = 2,
    ACTION_PROBE_POINT = 3,
    ACTION_PROBE_Z_TOP = 4,
    ACTION_MESSAGE = 5,
    ACTION_COMPLETE = 6,
} lv_probing_action_type_t;

static inline lv_obj_t* lv_cam_positioning_create(lv_obj_t *parent) {
    return 0;
}

static inline lv_obj_t* lv_cam_positioning_get_stream(lv_obj_t *obj) {
    return 0;
}

static inline void lv_cam_positioning_set_mode(lv_obj_t *obj, lv_cam_pos_mode_t mode) {
    (void)0;
}

static inline lv_cam_pos_mode_t lv_cam_positioning_get_mode(lv_obj_t *obj) {
    return 0;
}

static inline void lv_cam_positioning_clear_shapes(lv_obj_t *obj) {
    (void)0;
}

static inline bool lv_cam_positioning_has_grid(lv_obj_t *obj) {
    return 0;
}

static inline void lv_cam_positioning_wizard_cancel(lv_obj_t *obj) {
    (void)0;
}

static inline void lv_cam_positioning_clear_status_text(lv_obj_t *obj) {
    (void)0;
}

static inline lv_obj_t* lv_cam_stream_create(lv_obj_t *parent) {
    return 0;
}

static inline void lv_cam_stream_set_fit(lv_obj_t *obj, lv_cam_stream_fit_t fit) {
    (void)0;
}

static inline lv_cam_stream_fit_t lv_cam_stream_get_fit(lv_obj_t *obj) {
    return 0;
}

static inline void lv_cam_stream_set_show_info(lv_obj_t *obj, bool show) {
    (void)0;
}

static inline void lv_cam_stream_refresh(lv_obj_t *obj) {
    (void)0;
}

static inline lv_obj_t* lv_cam_stream_get_image_obj(lv_obj_t *obj) {
    return 0;
}

static inline lv_obj_t* lv_cnc_io_panel_create(lv_obj_t *parent) {
    return 0;
}

static inline void lv_cnc_io_panel_destroy(lv_obj_t *panel) {
    (void)0;
}

static inline void lv_cnc_io_panel_set_category(lv_obj_t *panel, io_category_t cat) {
    (void)0;
}

static inline lv_obj_t* lv_gcode_viewer_create(lv_obj_t *parent) {
    return 0;
}

static inline void lv_gcode_viewer_clear(lv_obj_t *obj) {
    (void)0;
}

static inline void lv_gcode_viewer_mark_done(lv_obj_t *obj) {
    (void)0;
}

static inline void lv_gcode_viewer_set_view(lv_obj_t *obj, lv_gcode_viewer_view_t mode) {
    (void)0;
}

static inline lv_gcode_viewer_view_t lv_gcode_viewer_get_view(lv_obj_t *obj) {
    return 0;
}

static inline void lv_gcode_viewer_next_view(lv_obj_t *obj) {
    (void)0;
}

static inline void lv_gcode_viewer_fit(lv_obj_t *obj) {
    (void)0;
}

static inline void lv_gcode_viewer_zoom(lv_obj_t *obj, float factor) {
    (void)0;
}

static inline void lv_gcode_viewer_set_grid(lv_obj_t *obj, bool show) {
    (void)0;
}

static inline void lv_gcode_viewer_set_limits(lv_obj_t *obj, bool show) {
    (void)0;
}

static inline void lv_gcode_viewer_set_position(lv_obj_t *obj, bool show) {
    (void)0;
}

static inline void lv_gcode_viewer_set_wcs_origin(lv_obj_t *obj, bool show) {
    (void)0;
}

static inline void lv_gcode_viewer_invalidate(lv_obj_t *obj) {
    (void)0;
}

static inline void lv_probing_wizard_register_stub_callbacks(lv_obj_t * obj) {
    (void)0;
}

static inline void lv_probing_wizard_set_z_top_deferred(lv_obj_t * obj, float z_top) {
    (void)0;
}

static inline void lv_probing_wizard_report_final_result_deferred(lv_obj_t * obj, float x, float y) {
    (void)0;
}

static inline void lv_probing_wizard_advance_step_deferred(lv_obj_t * obj) {
    (void)0;
}

static inline void lv_probing_wizard_set_active_step_deferred(lv_obj_t * obj, int8_t step_index) {
    (void)0;
}

static inline lv_obj_t* lv_probing_wizard_create(lv_obj_t * parent) {
    return 0;
}

static inline void lv_probing_wizard_set_mode(lv_obj_t * obj, lv_probing_wizard_mode_t mode, bool is_inside) {
    (void)0;
}

static inline void lv_probing_wizard_set_corner_type(lv_obj_t * obj, lv_probing_wizard_corner_t corner) {
    (void)0;
}

static inline void lv_probing_wizard_set_active_step(lv_obj_t * obj, int8_t step_index, bool defer_ui_update) {
    (void)0;
}

static inline void lv_probing_wizard_probe_intalled(lv_obj_t * obj) {
    (void)0;
}

static inline void lv_probing_wizard_set_connected(lv_obj_t * obj, bool connected) {
    (void)0;
}

static inline void lv_probing_wizard_report_probe_result(lv_obj_t * obj, uint8_t probe_index, float x, float y) {
    (void)0;
}

static inline void lv_probing_wizard_report_final_result(lv_obj_t * obj, float x, float y) {
    (void)0;
}

static inline void lv_probing_wizard_set_z_top(lv_obj_t * obj, float z_top) {
    (void)0;
}

static inline void lv_probing_wizard_advance_step(lv_obj_t * obj) {
    (void)0;
}

static inline float lv_probing_wizard_get_z_top(lv_obj_t * obj) {
    return 0;
}

static inline lv_probing_wizard_mode_t lv_probing_wizard_get_mode(lv_obj_t * obj) {
    return 0;
}

static inline bool lv_probing_wizard_get_is_inside(lv_obj_t * obj) {
    return 0;
}

static inline lv_probing_wizard_corner_t lv_probing_wizard_get_corner_type(lv_obj_t * obj) {
    return 0;
}

static inline lv_obj_t* lv_settings_create(lv_obj_t *parent) {
    return 0;
}

static inline void lv_settings_refresh(lv_obj_t *obj) {
    (void)0;
}

static inline void lv_settings_encoder_input(lv_obj_t *obj, int32_t diff) {
    (void)0;
}

static inline bool lv_settings_has_encoder_focus(lv_obj_t *obj) {
    return 0;
}

static inline void lv_settings_encoder_navigate(lv_obj_t *obj, int32_t dir) {
    (void)0;
}

#endif /* UI_STUBS_H_ */