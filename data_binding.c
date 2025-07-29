#include "data_binding.h"
#include "utils.h" // For print_warning
#include "debug_log.h"
#include "ui_sim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define MAX_STATES 128
#define MAX_OBSERVERS_PER_STATE 32

// --- Runtime Observer Structures ---

typedef struct {
    observer_update_type_t update_type;
    void* config;
    size_t config_len;
    void* default_value;
    lv_style_t* last_applied_style;
} ObserverConfig;

typedef struct {
    lv_obj_t* widget;
    ObserverConfig config;
} Observer;

typedef struct {
    char* state_name;
    uint32_t observer_count;
    Observer observers[MAX_OBSERVERS_PER_STATE];
} StateObserverMapping;

static StateObserverMapping state_observers[MAX_STATES];
static uint32_t state_observer_count = 0;

// --- Internal Structs for Dialog Action ---

// Stores the parsed configuration for a numeric dialog
typedef struct {
    float min_val;
    float max_val;
    float initial_val;
    char* format_str;
    char* text;
} NumericDialogConfig;

// Carries necessary data to the dialog's own event handlers
typedef struct {
    lv_obj_t* msgbox;
    lv_obj_t* slider;
    lv_obj_t* value_label;
    lv_obj_t* scale_min_label;
    lv_obj_t* scale_max_label;
    char* action_name;
    const NumericDialogConfig* dialog_config;
} DialogEventData;


// --- Unified Action User Data Structure ---

typedef struct {
    action_type_t type;
    const char* action_name;
    // For ACTION_TYPE_CYCLE
    binding_value_t* values;
    uint32_t value_count;
    uint32_t current_index;
    // For ACTION_TYPE_NUMERIC_DIALOG
    void* config_data;
} ActionUserData;


// --- Module-level variables ---
static data_binding_action_handler_t app_action_handler = NULL;
static void* app_user_data = NULL;

// --- Forward Declarations for Event Callbacks ---
static void generic_action_event_cb(lv_event_t* e);
static void free_action_user_data_cb(lv_event_t* e);
static void free_observer_config_cb(lv_event_t* e);
static void create_and_show_numeric_dialog(ActionUserData* user_data);

// --- Public API Implementation ---

void data_binding_init(void) {
    // This function needs to be safe to call multiple times for watch mode.
    // It must free any previously allocated memory.
    for (uint32_t i = 0; i < state_observer_count; i++) {
        free(state_observers[i].state_name);
        // Note: The individual observer configs are freed by the LV_EVENT_DELETE
        // callback attached to each widget. When lv_obj_clean is called, this
        // event is triggered for all children, ensuring no memory leaks.
    }

    memset(state_observers, 0, sizeof(state_observers));
    state_observer_count = 0;
    app_action_handler = NULL;
    app_user_data = NULL;
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Data binding system (re)initialized.");
}

void data_binding_register_action_handler(data_binding_action_handler_t handler, void* user_data) {
    app_action_handler = handler;
    app_user_data = user_data;
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Application action handler registered.");
}

static bool values_equal(const binding_value_t* v1, const binding_value_t* v2) {
    if (v1->type != v2->type) return false;
    switch(v1->type) {
        case BINDING_TYPE_BOOL: return v1->as.b_val == v2->as.b_val;
        case BINDING_TYPE_STRING: return strcmp(v1->as.s_val, v2->as.s_val) == 0;
        // For map keys, we assume exact float matches are intended.
        case BINDING_TYPE_FLOAT: return v1->as.f_val == v2->as.f_val;
        default: return false;
    }
}

void data_binding_notify_state_changed(const char* state_name, binding_value_t new_value) {
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Notification received for state: '%s'", state_name);
    for (uint32_t i = 0; i < state_observer_count; ++i) {
        if (strcmp(state_observers[i].state_name, state_name) == 0) {
            for(uint32_t j = 0; j < state_observers[i].observer_count; ++j) {
                Observer* obs = &state_observers[i].observers[j];
                if (!lv_obj_is_valid(obs->widget)) continue;

                switch (obs->config.update_type) {
                    case OBSERVER_TYPE_TEXT: {
                        char buf[128];
                        const char* fmt = (const char*)obs->config.config;
                        if (!fmt) fmt = "%s";

                        switch(new_value.type) {
                            case BINDING_TYPE_FLOAT:
                                if (strstr(fmt, "%d") || strstr(fmt, "%i") || strstr(fmt, "%u") || strstr(fmt, "%x")) {
                                    snprintf(buf, sizeof(buf), fmt, (int)round(new_value.as.f_val));
                                } else {
                                    snprintf(buf, sizeof(buf), fmt, new_value.as.f_val);
                                }
                                break;
                            case BINDING_TYPE_BOOL:   snprintf(buf, sizeof(buf), fmt, new_value.as.b_val ? "true" : "false"); break;
                            case BINDING_TYPE_STRING: snprintf(buf, sizeof(buf), fmt, new_value.as.s_val); break;
                            default:                  strncpy(buf, "N/A", sizeof(buf)); break;
                        }
                        lv_label_set_text(obs->widget, buf);
                        break;
                    }
                    case OBSERVER_TYPE_VALUE: {
                        if (new_value.type != BINDING_TYPE_FLOAT) {
                             print_warning("State '%s' sent non-numeric data to a 'value' binding.", state_name);
                             continue;
                        }

                        int32_t val = (int32_t)round(new_value.as.f_val);
                        lv_anim_enable_t anim = obs->config.config ? *(lv_anim_enable_t*)obs->config.config : LV_ANIM_ON;
                        const lv_obj_class_t * cls = lv_obj_get_class(obs->widget);

                        if (cls == &lv_bar_class) {
                            lv_bar_set_value(obs->widget, val, anim);
                        } else if (cls == &lv_slider_class) {
                            lv_slider_set_value(obs->widget, val, anim);
                        } else if (cls == &lv_arc_class) {
                            // lv_arc_set_value doesn't have an anim parameter
                            lv_arc_set_value(obs->widget, val);
                        } else {
                            print_warning("Widget of type <unknown class> does not support 'value' observation.");
                        }
                        break;
                    }
                    case OBSERVER_TYPE_VISIBLE:
                    case OBSERVER_TYPE_CHECKED:
                    case OBSERVER_TYPE_DISABLED: {
                        bool target_state;
                        if (obs->config.config_len > 0) { // Map-based
                            bool found = false;
                            binding_map_entry_t* map = (binding_map_entry_t*)obs->config.config;
                            for (size_t k = 0; k < obs->config.config_len; k++) {
                                if (values_equal(&map[k].key, &new_value)) {
                                    target_state = map[k].value.b_val;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found && obs->config.default_value) {
                                target_state = *(bool*)obs->config.default_value;
                            } else if (!found) {
                                continue;
                            }
                        } else { // Direct bool mapping
                            bool is_truthy = (new_value.type == BINDING_TYPE_BOOL && new_value.as.b_val) ||
                                             (new_value.type == BINDING_TYPE_FLOAT && new_value.as.f_val != 0.0f) ||
                                             (new_value.type == BINDING_TYPE_STRING && new_value.as.s_val && *new_value.as.s_val != '\0');
                            bool is_inverse = (obs->config.config == NULL) || !(*(bool*)obs->config.config);
                            target_state = is_inverse ? !is_truthy : is_inverse;
                        }

                        lv_obj_flag_t flag = 0;
                        lv_state_t state = 0;
                        if (obs->config.update_type == OBSERVER_TYPE_VISIBLE) flag = LV_OBJ_FLAG_HIDDEN;
                        if (obs->config.update_type == OBSERVER_TYPE_DISABLED) state = LV_STATE_DISABLED;
                        if (obs->config.update_type == OBSERVER_TYPE_CHECKED) state = LV_STATE_CHECKED;

                        if (flag) { // Visibility is an obj_flag
                             if (target_state) lv_obj_clear_flag(obs->widget, flag);
                             else lv_obj_add_flag(obs->widget, flag);
                        } else if (state) { // Others are states
                             if (target_state) lv_obj_add_state(obs->widget, state);
                             else lv_obj_clear_state(obs->widget, state);
                        }
                        break;
                    }
                    case OBSERVER_TYPE_STYLE: {
                        // ** THE FIX **: Do not apply custom styles if the object is disabled,
                        // as LVGL's disabled style should take precedence.
                        if (lv_obj_has_state(obs->widget, LV_STATE_DISABLED)) {
                            // If we previously applied a style, remove it now that the widget is disabled.
                            if(obs->config.last_applied_style) {
                                lv_obj_remove_style(obs->widget, obs->config.last_applied_style, 0);
                                obs->config.last_applied_style = NULL;
                            }
                            continue;
                        }

                        lv_style_t* style_to_apply = NULL;
                        binding_map_entry_t* map = (binding_map_entry_t*)obs->config.config;
                        bool found = false;
                        for (size_t k = 0; k < obs->config.config_len; k++) {
                             if (values_equal(&map[k].key, &new_value)) {
                                style_to_apply = (lv_style_t*)map[k].value.p_val;
                                found = true;
                                break;
                            }
                        }
                        if (!found && obs->config.default_value) {
                            style_to_apply = (lv_style_t*)obs->config.default_value;
                        }

                        if (obs->config.last_applied_style != style_to_apply) {
                            if (obs->config.last_applied_style) {
                                lv_obj_remove_style(obs->widget, obs->config.last_applied_style, 0);
                            }
                            if (style_to_apply) {
                                lv_obj_add_style(obs->widget, style_to_apply, 0);
                            }
                            obs->config.last_applied_style = style_to_apply;
                        }
                        break;
                    }
                }
            }
            return;
        }
    }
    DEBUG_LOG(LOG_MODULE_DATABINDING, "No observers found for state: '%s'", state_name);
}


void data_binding_add_observer(const char* state_name, lv_obj_t* widget,
                               observer_update_type_t update_type,
                               const void* config, size_t config_len, const void* default_value)
{
    if (!state_name || !widget) return;

    int state_idx = -1;
    for (uint32_t i = 0; i < state_observer_count; ++i) {
        if (strcmp(state_observers[i].state_name, state_name) == 0) {
            state_idx = i;
            break;
        }
    }

    if (state_idx == -1) {
        if (state_observer_count >= MAX_STATES) {
            print_warning("Max number of observed states (%d) reached.", MAX_STATES, state_name);
            return;
        }
        state_idx = state_observer_count++;
        state_observers[state_idx].state_name = strdup(state_name);
        state_observers[state_idx].observer_count = 0;
    }

    StateObserverMapping* mapping = &state_observers[state_idx];
    if (mapping->observer_count >= MAX_OBSERVERS_PER_STATE) {
        print_warning("Max observers for state '%s' reached.", state_name);
        return;
    }

    Observer* obs = &mapping->observers[mapping->observer_count++];
    obs->widget = widget;
    obs->config.update_type = update_type;
    obs->config.config_len = config_len;
    obs->config.last_applied_style = NULL;

    // Deep copy config data
    if (update_type == OBSERVER_TYPE_VALUE) {
        if (config) {
            lv_anim_enable_t* anim = malloc(sizeof(lv_anim_enable_t));
            if (!anim) render_abort("Failed to allocate observer anim config");
            *anim = *(const lv_anim_enable_t*)config;
            obs->config.config = anim;
        } else {
            obs->config.config = NULL; // Use default
        }
    } else if (config_len > 0) { // It's a map
        size_t map_size = config_len * sizeof(binding_map_entry_t);
        binding_map_entry_t* copied_map = malloc(map_size);
        if (!copied_map) render_abort("Failed to allocate observer map config");
        memcpy(copied_map, config, map_size);
        for (size_t i = 0; i < config_len; i++) {
            if (copied_map[i].key.type == BINDING_TYPE_STRING) {
                copied_map[i].key.as.s_val = strdup(copied_map[i].key.as.s_val);
            }
        }
        obs->config.config = copied_map;
    } else { // It's a format string or a bool*
        if (update_type == OBSERVER_TYPE_TEXT) {
            obs->config.config = config ? strdup((const char*)config) : NULL;
        } else if (config) {
            bool* b = malloc(sizeof(bool));
            if (!b) render_abort("Failed to allocate observer bool config");
            *b = *(const bool*)config;
            obs->config.config = b;
        } else {
            obs->config.config = NULL;
        }
    }

    if (default_value) {
         if (update_type == OBSERVER_TYPE_STYLE) {
            obs->config.default_value = (void*)default_value;
         } else {
            bool* b = malloc(sizeof(bool));
            if (!b) render_abort("Failed to allocate observer default value");
            *b = *(const bool*)default_value;
            obs->config.default_value = b;
         }
    } else {
        obs->config.default_value = NULL;
    }

    lv_obj_add_event_cb(widget, free_observer_config_cb, LV_EVENT_DELETE, &obs->config);

    DEBUG_LOG(LOG_MODULE_DATABINDING, "Added observer for state '%s' to widget %p.", state_name, (void*)widget);
}

void data_binding_add_action(lv_obj_t* widget, const char* action_name, action_type_t type, const binding_value_t* cycle_values, uint32_t cycle_value_count, const void* config_data) {
    if (!widget || !action_name) return;

    ActionUserData* user_data = calloc(1, sizeof(ActionUserData));
    if (!user_data) render_abort("Failed to allocate action user data");

    user_data->type = type;
    user_data->action_name = strdup(action_name);
    if (!user_data->action_name) {
        free(user_data);
        render_abort("Failed to duplicate action name");
        return;
    }

    lv_event_cb_t cb = generic_action_event_cb;
    lv_event_code_t code = LV_EVENT_CLICKED;

    if (type == ACTION_TYPE_TOGGLE) {
        code = LV_EVENT_VALUE_CHANGED;
    } else if (type == ACTION_TYPE_CYCLE) {
        if (!cycle_values || cycle_value_count == 0) {
            free((void*)user_data->action_name);
            free(user_data);
            return;
        }

        binding_value_t* copied_values = malloc(cycle_value_count * sizeof(binding_value_t));
        if (!copied_values) render_abort("Failed to allocate memory for cycle action values.");

        for (uint32_t i = 0; i < cycle_value_count; i++) {
            copied_values[i] = cycle_values[i];
            if (cycle_values[i].type == BINDING_TYPE_STRING && cycle_values[i].as.s_val) {
                copied_values[i].as.s_val = strdup(cycle_values[i].as.s_val);
                if (!copied_values[i].as.s_val) render_abort("Failed to duplicate string in cycle action values.");
            }
        }

        user_data->values = copied_values;
        user_data->value_count = cycle_value_count;
    } else if (type == ACTION_TYPE_NUMERIC_DIALOG) {
        if (config_data) {
            NumericDialogConfig* new_config = calloc(1, sizeof(NumericDialogConfig));
            if (!new_config) render_abort("Failed to allocate numeric dialog config.");
            const NumericDialogConfig* src_config = (const NumericDialogConfig*)config_data;
            new_config->min_val = src_config->min_val;
            new_config->max_val = src_config->max_val;
            new_config->initial_val = src_config->initial_val;
            if (src_config->format_str) new_config->format_str = strdup(src_config->format_str);
            if (src_config->text) new_config->text = strdup(src_config->text);
            user_data->config_data = new_config;
        }
    }

    lv_obj_add_event_cb(widget, cb, code, user_data);
    if (code == LV_EVENT_CLICKED) {
      lv_obj_add_flag(widget, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(widget, free_action_user_data_cb, LV_EVENT_DELETE, user_data);
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Added action '%s' (type %d) to widget %p.", action_name, type, (void*)widget);
}


// --- Event Callback Implementations ---

static void free_observer_config_cb(lv_event_t* e) {
    ObserverConfig* config = lv_event_get_user_data(e);
    if (config) {
        if (config->update_type != OBSERVER_TYPE_VALUE && config->config_len > 0) { // It's a map
            binding_map_entry_t* map = config->config;
            for (size_t i = 0; i < config->config_len; i++) {
                if (map[i].key.type == BINDING_TYPE_STRING) {
                    free((void*)map[i].key.as.s_val);
                }
            }
        }
        free(config->config);
        free(config->default_value);
        config->config = NULL;
        config->default_value = NULL;
        DEBUG_LOG(LOG_MODULE_DATABINDING, "Freed observer config for widget %p.", (void*)lv_event_get_target(e));
    }
}

static void free_action_user_data_cb(lv_event_t* e) {
    ActionUserData* user_data = lv_event_get_user_data(e);
    if (user_data) {
        free((void*)user_data->action_name);
        if (user_data->type == ACTION_TYPE_CYCLE && user_data->values) {
            for (uint32_t i = 0; i < user_data->value_count; i++) {
                if (user_data->values[i].type == BINDING_TYPE_STRING && user_data->values[i].as.s_val) {
                    free((void*)user_data->values[i].as.s_val);
                }
            }
            free(user_data->values);
        } else if (user_data->type == ACTION_TYPE_NUMERIC_DIALOG && user_data->config_data) {
            NumericDialogConfig* cfg = (NumericDialogConfig*)user_data->config_data;
            free(cfg->format_str);
            free(cfg->text);
            free(cfg);
        }
        DEBUG_LOG(LOG_MODULE_DATABINDING, "Freeing user_data for widget %p.", (void*)lv_event_get_target(e));
        free(user_data);
    }
}

// --- Numeric Dialog Implementation ---

static void update_scale_labels(DialogEventData* data) {
    int32_t min_val = lv_slider_get_min_value(data->slider);
    int32_t max_val = lv_slider_get_max_value(data->slider);
    lv_label_set_text_fmt(data->scale_min_label, "%" LV_PRId32, min_val);
    lv_label_set_text_fmt(data->scale_max_label, "%" LV_PRId32, max_val);
}

static void slider_released_cb(lv_event_t* e) {
    DialogEventData* data = lv_event_get_user_data(e);
    lv_obj_t* slider = data->slider;

    int32_t current_val = lv_slider_get_value(slider);
    int32_t min_val = lv_slider_get_min_value(slider);
    int32_t max_val = lv_slider_get_max_value(slider);

    if (current_val == max_val) {
        int32_t new_max = max_val == 0 ? 100 : max_val * 2;
        lv_slider_set_range(slider, min_val, new_max);
        lv_slider_set_value(slider, current_val, LV_ANIM_OFF); // Restore value
        update_scale_labels(data);
    } else if (current_val == min_val) {
        int32_t range = max_val - min_val;
        if (range > 1) { // Only shrink if there's room to do so
            int32_t new_max = min_val + (range / 2);
            if (new_max > min_val) {
                 lv_slider_set_range(slider, min_val, new_max);
                 lv_slider_set_value(slider, current_val, LV_ANIM_OFF); // Restore value
                 update_scale_labels(data);
            }
        }
    }
}

static void update_slider_label(DialogEventData* data) {
    int32_t value = lv_slider_get_value(data->slider);
    const char* fmt = data->dialog_config->format_str ? data->dialog_config->format_str : "%d";
    lv_label_set_text_fmt(data->value_label, fmt, value);
}

static void slider_value_changed_cb(lv_event_t* e) {
    DialogEventData* data = lv_event_get_user_data(e);
    update_slider_label(data);
}

static void mb_ok_event_cb(lv_event_t* e) {
    DialogEventData* data = lv_event_get_user_data(e);

    float value = (float)lv_slider_get_value(data->slider);
    binding_value_t final_value = {.type = BINDING_TYPE_FLOAT, .as.f_val = value};

    DEBUG_LOG(LOG_MODULE_DATABINDING, "Numeric dialog OK, dispatching action '%s' with value %f.", data->action_name, final_value.as.f_val);
    if (app_action_handler) {
        app_action_handler(data->action_name, final_value, app_user_data);
    }

    // This callback handles both OK and Cancel, so we always close.
    lv_msgbox_close(data->msgbox);
}

static void dialog_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    DialogEventData* data = lv_event_get_user_data(e);

    if (code == LV_EVENT_DELETE) {
        // The msgbox is being deleted, free our associated data
        free(data->action_name);
        free(data);
        DEBUG_LOG(LOG_MODULE_DATABINDING, "Numeric dialog cleaned up.");
    }
}

static void mb_close_cb(lv_event_t *e) {
  lv_obj_t *mbox = (lv_obj_t *) lv_event_get_user_data(e);
  lv_msgbox_close(mbox);
}

static void create_and_show_numeric_dialog(ActionUserData* user_data) {
    NumericDialogConfig* cfg = (NumericDialogConfig*)user_data->config_data;
    if (!cfg) {
        print_warning("Numeric dialog action for '%s' triggered without config.", user_data->action_name);
        return;
    }

    // --- Create Dialog ---
    lv_obj_t* mbox = lv_msgbox_create(NULL);

    // --- Add custom content ---
    lv_obj_t* content = lv_msgbox_get_content(mbox);
    lv_obj_set_size(content, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(content, 10, 0);

    lv_obj_t *cont = lv_obj_create(content);
    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER,  LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 5, 0);

    lv_obj_t* title_label = lv_label_create(cont);
    lv_label_set_text(title_label, cfg->text ? cfg->text : "Enter value:");
    lv_obj_t* value_label = lv_label_create(cont);

    lv_obj_t* slider = lv_slider_create(cont);
    lv_obj_set_width(slider, lv_pct(100));
    lv_slider_set_orientation(slider, LV_SLIDER_ORIENTATION_HORIZONTAL);
    lv_slider_set_range(slider, (int32_t)cfg->min_val, (int32_t)cfg->max_val);
    lv_slider_set_value(slider, (int32_t)cfg->initial_val, LV_ANIM_OFF);

    // --- Add Scale Labels ---
    lv_obj_t* scale_cont = lv_obj_create(cont);
    lv_obj_set_width(scale_cont, lv_pct(100));
    lv_obj_set_height(scale_cont, LV_SIZE_CONTENT);
    lv_obj_remove_style(scale_cont, NULL, LV_PART_SCROLLBAR | LV_STATE_ANY);
    lv_obj_set_style_bg_opa(scale_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scale_cont, 0, 0);
    lv_obj_set_style_pad_all(scale_cont, 0, 0);
    lv_obj_set_flex_flow(scale_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scale_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* min_label = lv_label_create(scale_cont);
    lv_obj_t* max_label = lv_label_create(scale_cont);
    lv_obj_set_style_text_color(min_label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_color(max_label, lv_color_hex(0x808080), 0);

    // --- Add footer buttons ---
    lv_obj_t *ok_btn = lv_msgbox_add_footer_button(mbox, "OK");
    lv_obj_set_flex_grow(ok_btn, 1);
    lv_obj_t *cncl_btn = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_set_flex_grow(cncl_btn, 1);
    lv_obj_center(mbox);

    // --- Create and link event data ---
    DialogEventData* data = calloc(1, sizeof(DialogEventData));
    if(!data) render_abort("Failed to alloc DialogEventData");

    data->msgbox = mbox;
    data->slider = slider;
    data->value_label = value_label;
    data->scale_min_label = min_label;
    data->scale_max_label = max_label;
    data->action_name = strdup(user_data->action_name);
    data->dialog_config = cfg;

    // Add event handlers
    lv_obj_add_event_cb(slider, slider_value_changed_cb, LV_EVENT_VALUE_CHANGED, data);
    lv_obj_add_event_cb(slider, slider_released_cb, LV_EVENT_RELEASED, data);
    lv_obj_add_event_cb(mbox, dialog_event_cb, LV_EVENT_ALL, data);
    lv_obj_add_event_cb(ok_btn, mb_ok_event_cb, LV_EVENT_CLICKED, data);
    lv_obj_add_event_cb(cncl_btn, mb_close_cb, LV_EVENT_CLICKED, mbox);

    // Initial updates
    update_slider_label(data);
    update_scale_labels(data);
}


static void generic_action_event_cb(lv_event_t* e) {
    ActionUserData* user_data = lv_event_get_user_data(e);
    binding_value_t val = {.type = BINDING_TYPE_NULL};

    // --- INTERCEPTION for dialog action ---
    if (user_data->type == ACTION_TYPE_NUMERIC_DIALOG) {
        DEBUG_LOG(LOG_MODULE_DATABINDING, "Intercepting action '%s' to show numeric dialog.", user_data->action_name);
        create_and_show_numeric_dialog(user_data);
        return; // Stop processing, do not call app handler.
    }

    // --- NORMAL DISPATCH for other actions ---
    switch(user_data->type) {
        case ACTION_TYPE_TRIGGER:
            break;
        case ACTION_TYPE_TOGGLE:
            val.type = BINDING_TYPE_BOOL;
            val.as.b_val = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
            break;
        case ACTION_TYPE_CYCLE:
            if (user_data->value_count > 0) {
                val = user_data->values[user_data->current_index];
                user_data->current_index = (user_data->current_index + 1) % user_data->value_count;
            }
            break;
        case ACTION_TYPE_NUMERIC_DIALOG: // Should not be reached
            break;
    }

    DEBUG_LOG(LOG_MODULE_DATABINDING, "Dispatching action: '%s'", user_data->action_name);
    if (app_action_handler) {
        app_action_handler(user_data->action_name, val, app_user_data);
    }
}