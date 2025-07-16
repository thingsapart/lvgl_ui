#include "data_binding.h"
#include "utils.h" // For print_warning
#include "debug_log.h"
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

// --- Unified Action User Data Structure ---

typedef struct {
    action_type_t type;
    const char* action_name;
    binding_value_t* values;
    uint32_t value_count;
    uint32_t current_index;
} ActionUserData;


// --- Module-level variables ---
static data_binding_action_handler_t app_action_handler = NULL;

// --- Forward Declarations for Event Callbacks ---
static void generic_action_event_cb(lv_event_t* e);
static void free_action_user_data_cb(lv_event_t* e);
static void free_observer_config_cb(lv_event_t* e);

// --- Public API Implementation ---

void data_binding_init(void) {
    memset(state_observers, 0, sizeof(state_observers));
    state_observer_count = 0;
    app_action_handler = NULL;
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Data binding system initialized.");
}

void data_binding_register_action_handler(data_binding_action_handler_t handler) {
    app_action_handler = handler;
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
                        if (new_value.type == BINDING_TYPE_FLOAT) {
                           lv_slider_set_value(obs->widget, (int32_t)new_value.as.f_val, LV_ANIM_ON);
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
                            bool is_inverse = !(*(bool*)obs->config.config);
                            target_state = is_inverse ? !is_truthy : is_truthy;
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
    if (config_len > 0) { // It's a map
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
        if (update_type == OBSERVER_TYPE_TEXT || update_type == OBSERVER_TYPE_VALUE) {
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

void data_binding_add_action(lv_obj_t* widget, const char* action_name, action_type_t type, const binding_value_t* cycle_values, uint32_t cycle_value_count) {
    if (!widget || !action_name) return;

    ActionUserData* user_data = calloc(1, sizeof(ActionUserData));
    if (!user_data) render_abort("Failed to allocate action user data");

    user_data->type = type;
    user_data->action_name = action_name;

    lv_event_cb_t cb = generic_action_event_cb;
    lv_event_code_t code = LV_EVENT_CLICKED;

    if (type == ACTION_TYPE_TOGGLE) {
        code = LV_EVENT_VALUE_CHANGED;
    } else if (type == ACTION_TYPE_CYCLE) {
        if (!cycle_values || cycle_value_count == 0) {
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
    }
    
    lv_obj_add_event_cb(widget, cb, code, user_data);
    lv_obj_add_event_cb(widget, free_action_user_data_cb, LV_EVENT_DELETE, user_data);
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Added action '%s' (type %d) to widget %p.", action_name, type, (void*)widget);
}


// --- Event Callback Implementations ---

static void free_observer_config_cb(lv_event_t* e) {
    ObserverConfig* config = lv_event_get_user_data(e);
    if (config) {
        if (config->config_len > 0) { // It's a map
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
        if (user_data->type == ACTION_TYPE_CYCLE && user_data->values) {
            for (uint32_t i = 0; i < user_data->value_count; i++) {
                if (user_data->values[i].type == BINDING_TYPE_STRING && user_data->values[i].as.s_val) {
                    free((void*)user_data->values[i].as.s_val);
                }
            }
            free(user_data->values);
        }
        DEBUG_LOG(LOG_MODULE_DATABINDING, "Freeing user_data for widget %p.", (void*)lv_event_get_target(e));
        free(user_data);
    }
}

static void generic_action_event_cb(lv_event_t* e) {
    if (!app_action_handler) return;

    ActionUserData* user_data = lv_event_get_user_data(e);
    binding_value_t val = {.type = BINDING_TYPE_NULL};

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
    }
    
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Dispatching action: '%s'", user_data->action_name);
    if (app_action_handler) {
        app_action_handler(user_data->action_name, val);
    }
}
