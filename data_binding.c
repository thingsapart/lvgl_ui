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

// --- Action User Data Structures ---

typedef struct {
    const char* action_name;
} TriggerActionUserData;

typedef struct {
    const char* action_name;
    bool current_state;
} ToggleActionUserData;

typedef struct {
    const char* action_name;
    const binding_value_t* values; // Points to static or heap-allocated array
    bool values_are_heap_allocated; // Flag to indicate if we need to free `values`
    uint32_t value_count;
    uint32_t current_index;
} CycleActionUserData;

// --- Module-level variables ---
static data_binding_action_handler_t app_action_handler = NULL;

// --- Forward Declarations for Event Callbacks ---
static void trigger_event_cb(lv_event_t* e);
static void toggle_event_cb(lv_event_t* e);
static void cycle_event_cb(lv_event_t* e);
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
                            char buf[128];
                            const char* fmt = current->observer.format_string ? current->observer.format_string : "%s";
                            switch(new_value.type) {
                                case BINDING_TYPE_INT:    snprintf(buf, sizeof(buf), fmt, new_value.as.i_val); break;
                                case BINDING_TYPE_FLOAT:  snprintf(buf, sizeof(buf), fmt, new_value.as.f_val); break;
                                case BINDING_TYPE_BOOL:   snprintf(buf, sizeof(buf), fmt, new_value.as.b_val ? "ON" : "OFF"); break;
                                case BINDING_TYPE_STRING: snprintf(buf, sizeof(buf), fmt, new_value.as.s_val); break;
                                default:                  strncpy(buf, "N/A", sizeof(buf)-1); buf[sizeof(buf)-1] = '\0'; break;
                            }
                            lv_label_set_text(current->observer.widget, buf);
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
                            if (new_value.type == BINDING_TYPE_INT || new_value.type == BINDING_TYPE_FLOAT) {
                                int32_t val = (new_value.type == BINDING_TYPE_INT) ? new_value.as.i_val : (int32_t)new_value.as.f_val;
                                lv_slider_set_value(current->observer.widget, val, LV_ANIM_ON);
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
    void* user_data = NULL;
    lv_event_cb_t cb = NULL;
    lv_event_code_t code = LV_EVENT_CLICKED;

    switch(type) {
        case ACTION_TYPE_TRIGGER: {
            user_data = malloc(sizeof(TriggerActionUserData));
            ((TriggerActionUserData*)user_data)->action_name = action_name; // Static string, no copy needed
            cb = trigger_event_cb;
            break;
        }
        case ACTION_TYPE_TOGGLE: {
            user_data = malloc(sizeof(ToggleActionUserData));
            ((ToggleActionUserData*)user_data)->action_name = action_name;
            ((ToggleActionUserData*)user_data)->current_state = lv_obj_has_state(widget, LV_STATE_CHECKED);
            cb = toggle_event_cb;
            code = LV_EVENT_VALUE_CHANGED;
            break;
        }
        case ACTION_TYPE_CYCLE: {
            if (!cycle_values || cycle_value_count == 0) return;
            CycleActionUserData* cycle_data = malloc(sizeof(CycleActionUserData));
            cycle_data->action_name = action_name;
            cycle_data->values = cycle_values;
            cycle_data->values_are_heap_allocated = (lv_obj_get_screen(widget) != lv_screen_active()); // Heuristic: if renderer is running, assume heap
            cycle_data->value_count = cycle_value_count;
            cycle_data->current_index = 0; // Always start at the first value
            user_data = cycle_data;
            cb = cycle_event_cb;
            break;
        }
    }
    
    if (user_data && cb) {
        lv_obj_add_event_cb(widget, cb, code, user_data);
        // Add a cleanup handler for all action types that have user_data
        lv_obj_add_event_cb(widget, free_action_user_data_cb, LV_EVENT_DELETE, user_data);
        DEBUG_LOG(LOG_MODULE_DATABINDING, "Added action '%s' (type %d) to widget %p.", action_name, type, (void*)widget);
    }
}


// --- Event Callback Implementations ---

static void free_action_user_data_cb(lv_event_t* e) {
    void* user_data = lv_event_get_user_data(e);
    if (user_data) {
        // Special handling for cycle actions with heap-allocated value arrays
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_DELETE) {
             CycleActionUserData* cycle_data = user_data;
             if (cycle_data->values_are_heap_allocated && cycle_data->values) {
                 DEBUG_LOG(LOG_MODULE_DATABINDING, "Freeing heap-allocated cycle values for action '%s'", cycle_data->action_name);
                 free((void*)cycle_data->values);
             }
        }
        DEBUG_LOG(LOG_MODULE_DATABINDING, "Freeing user_data for widget %p.", (void*)lv_event_get_target(e));
        free(user_data);
    }
}

static void trigger_event_cb(lv_event_t* e) {
    if (!app_action_handler) return;
    TriggerActionUserData* user_data = lv_event_get_user_data(e);
    binding_value_t val = {.type = BINDING_TYPE_NULL};
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Dispatching TRIGGER action: '%s'", user_data->action_name);
    app_action_handler(user_data->action_name, val);
}

static void toggle_event_cb(lv_event_t* e) {
    if (!app_action_handler) return;
    ToggleActionUserData* user_data = lv_event_get_user_data(e);
    lv_obj_t* widget = lv_event_get_target(e);
    user_data->current_state = lv_obj_has_state(widget, LV_STATE_CHECKED);
    binding_value_t val = {.type = BINDING_TYPE_BOOL, .as.b_val = user_data->current_state};
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Dispatching TOGGLE action: '%s', new state: %s", user_data->action_name, val.as.b_val ? "ON" : "OFF");
    app_action_handler(user_data->action_name, val);
}

static void cycle_event_cb(lv_event_t* e) {
    if (!app_action_handler) return;
    CycleActionUserData* user_data = lv_event_get_user_data(e);
    
    // Increment and wrap index
    user_data->current_index = (user_data->current_index + 1) % user_data->value_count;
    
    // Get the new value from the list
    binding_value_t new_val = user_data->values[user_data->current_index];
    
    // Dispatch the action
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Dispatching CYCLE action: '%s', index: %u", user_data->action_name, user_data->current_index);
    app_action_handler(user_data->action_name, new_val);
}
