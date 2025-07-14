#include "data_binding.h"
#include "utils.h" // For print_warning
#include "debug_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_STATES 64

// --- Observer Structures ---

typedef struct {
    lv_obj_t* widget;
    observer_update_type_t update_type;
    char* format_string; // Owned by this struct
} Observer;

typedef struct ObserverListNode {
    Observer observer;
    struct ObserverListNode* next;
} ObserverListNode;

typedef struct {
    char* state_name; // Owned by this struct
    ObserverListNode* head;
} StateObserverMapping;

static StateObserverMapping state_observers[MAX_STATES];
static uint32_t state_observer_count = 0;

// --- Unified Action User Data Structure ---

typedef struct {
    action_type_t type;
    const char* action_name;        // Persistent string from IR
    binding_value_t* values;        // For cycle actions, heap-allocated and owned by this struct
    uint32_t value_count;           // For cycle actions
    uint32_t current_index;         // For cycle actions
} ActionUserData;


// --- Module-level variables ---
static data_binding_action_handler_t app_action_handler = NULL;

// --- Forward Declarations for Event Callbacks ---
static void generic_action_event_cb(lv_event_t* e);
static void free_action_user_data_cb(lv_event_t* e);


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

void data_binding_notify_state_changed(const char* state_name, binding_value_t new_value) {
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Notification received for state: '%s'", state_name);
    for (uint32_t i = 0; i < state_observer_count; ++i) {
        if (strcmp(state_observers[i].state_name, state_name) == 0) {
            ObserverListNode* current = state_observers[i].head;
            while (current) {
                if (lv_obj_is_valid(current->observer.widget)) {
                    switch (current->observer.update_type) {
                        case OBSERVER_TYPE_LABEL_TEXT: {
                            char* buf = NULL;
                            int len = 0;
                            const char* fmt = current->observer.format_string ? current->observer.format_string : "%s";
                            
                            // Determine required buffer size
                            switch(new_value.type) {
                                case BINDING_TYPE_FLOAT:  len = snprintf(NULL, 0, fmt, new_value.as.f_val); break;
                                case BINDING_TYPE_BOOL:   len = snprintf(NULL, 0, fmt, new_value.as.b_val ? "ON" : "OFF"); break;
                                case BINDING_TYPE_STRING: len = snprintf(NULL, 0, fmt, new_value.as.s_val); break;
                                default:                  len = 3; break; // "N/A"
                            }
                            
                            if (len >= 0) {
                                buf = malloc(len + 1);
                                if(buf) {
                                    // Format into the allocated buffer
                                    switch(new_value.type) {
                                        case BINDING_TYPE_FLOAT:  snprintf(buf, len + 1, fmt, new_value.as.f_val); break;
                                        case BINDING_TYPE_BOOL:   snprintf(buf, len + 1, fmt, new_value.as.b_val ? "ON" : "OFF"); break;
                                        case BINDING_TYPE_STRING: snprintf(buf, len + 1, fmt, new_value.as.s_val); break;
                                        default:                  strcpy(buf, "N/A"); break;
                                    }
                                    lv_label_set_text(current->observer.widget, buf);
                                    free(buf);
                                }
                            }
                            break;
                        }
                        case OBSERVER_TYPE_SWITCH_STATE:
                            if (new_value.type == BINDING_TYPE_BOOL) {
                                if (new_value.as.b_val) lv_obj_add_state(current->observer.widget, LV_STATE_CHECKED);
                                else lv_obj_clear_state(current->observer.widget, LV_STATE_CHECKED);
                            } else {
                                print_warning("Type mismatch: switch observer for '%s' received non-bool value.", state_name);
                            }
                            break;
                        case OBSERVER_TYPE_SLIDER_VALUE:
                            if (new_value.type == BINDING_TYPE_FLOAT) {
                                lv_slider_set_value(current->observer.widget, (int32_t)new_value.as.f_val, LV_ANIM_ON);
                            } else {
                                print_warning("Type mismatch: slider observer for '%s' received non-numeric value.", state_name);
                            }
                            break;
                    }
                }
                current = current->next;
            }
            return; // Found and processed state
        }
    }
    DEBUG_LOG(LOG_MODULE_DATABINDING, "No observers found for state: '%s'", state_name);
}

// --- Internal API Implementation for Generated Code ---

void data_binding_add_observer(const char* state_name, lv_obj_t* widget, observer_update_type_t update_type, const char* format) {
    if (!state_name || !widget) return;

    // Find existing state mapping
    int state_idx = -1;
    for (uint32_t i = 0; i < state_observer_count; ++i) {
        if (strcmp(state_observers[i].state_name, state_name) == 0) {
            state_idx = i;
            break;
        }
    }

    // If not found, create a new one
    if (state_idx == -1) {
        if (state_observer_count >= MAX_STATES) {
            print_warning("Max number of observed states (%d) reached. Cannot add '%s'.", MAX_STATES, state_name);
            return;
        }
        state_idx = state_observer_count++;
        state_observers[state_idx].state_name = strdup(state_name);
        state_observers[state_idx].head = NULL;
    }

    // Add the new observer to the list for this state
    ObserverListNode* new_node = malloc(sizeof(ObserverListNode));
    if (!new_node) {
        render_abort("Failed to allocate observer node.");
    }
    new_node->observer.widget = widget;
    new_node->observer.update_type = update_type;
    new_node->observer.format_string = format ? strdup(format) : NULL;
    new_node->next = state_observers[state_idx].head;
    state_observers[state_idx].head = new_node;
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
        
        // Deep copy the cycle values array to take ownership.
        binding_value_t* copied_values = malloc(cycle_value_count * sizeof(binding_value_t));
        if (!copied_values) render_abort("Failed to allocate memory for cycle action values.");
        
        for (uint32_t i = 0; i < cycle_value_count; i++) {
            copied_values[i] = cycle_values[i];
            if (cycle_values[i].type == BINDING_TYPE_STRING && cycle_values[i].as.s_val) {
                // Also copy the string itself
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

static void free_action_user_data_cb(lv_event_t* e) {
    ActionUserData* user_data = lv_event_get_user_data(e);
    if (user_data) {
        if (user_data->type == ACTION_TYPE_CYCLE && user_data->values) {
            // Free the deep-copied data
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
            DEBUG_LOG(LOG_MODULE_DATABINDING, "Dispatching TRIGGER action: '%s'", user_data->action_name);
            // val is already NULL, which is correct.
            break;
        case ACTION_TYPE_TOGGLE:
            val.type = BINDING_TYPE_BOOL;
            val.as.b_val = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
            DEBUG_LOG(LOG_MODULE_DATABINDING, "Dispatching TOGGLE action: '%s', new state: %s", user_data->action_name, val.as.b_val ? "ON" : "OFF");
            break;
        case ACTION_TYPE_CYCLE:
            if (user_data->value_count > 0) {
                // Dispatch the current value first.
                val = user_data->values[user_data->current_index];
                DEBUG_LOG(LOG_MODULE_DATABINDING, "Dispatching CYCLE action: '%s', index: %u", user_data->action_name, user_data->current_index);
                // Then increment the index for the *next* call.
                user_data->current_index = (user_data->current_index + 1) % user_data->value_count;
            }
            break;
    }

    if (app_action_handler) {
        app_action_handler(user_data->action_name, val);
    }
}
