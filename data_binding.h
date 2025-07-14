#ifndef DATA_BINDING_H
#define DATA_BINDING_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

// --- Public Data Types ---

/**
 * @brief Enum for the types of values that can be passed through the binding system.
 * Numeric types have been consolidated into BINDING_TYPE_FLOAT.
 */
typedef enum {
    BINDING_TYPE_NULL,
    BINDING_TYPE_FLOAT,
    BINDING_TYPE_BOOL,
    BINDING_TYPE_STRING,
} binding_value_type_t;

/**
 * @brief A tagged union for passing typed data.
 */
typedef struct {
    binding_value_type_t type;
    union {
        float f_val;
        bool b_val;
        const char* s_val; // Assumed to be a persistent string
    } as;
} binding_value_t;

/**
 * @brief Enum defining the different types of actions a widget can perform.
 */
typedef enum {
    ACTION_TYPE_TRIGGER, // Simple, stateless event
    ACTION_TYPE_TOGGLE,  // Toggles between bool true/false (0/1)
    ACTION_TYPE_CYCLE,   // Cycles through a list of predefined values
    // ACTION_TYPE_SLIDER_MODAL, // Future enhancement
} action_type_t;

/**
 * @brief A function pointer for the application's main action handler.
 * This single function will receive all actions triggered by the UI.
 */
typedef void (*data_binding_action_handler_t)(const char* action_name, binding_value_t value);


// --- Public API for the Main Application ---

/**
 * @brief Initializes the data binding system. Must be called once.
 */
void data_binding_init(void);

/**
 * @brief Registers the application's single action handler function.
 * @param handler A pointer to the function that will process UI actions.
 */
void data_binding_register_action_handler(data_binding_action_handler_t handler);

/**
 * @brief Notifies the UI that a piece of the application's state has changed.
 * The data binding library will find all widgets observing this state and update them.
 * @param state_name The unique name of the state variable (e.g., "position::x").
 * @param new_value The new value of the state in a binding_value_t.
 */
void data_binding_notify_state_changed(const char* state_name, binding_value_t new_value);


// --- Internal API for Generated Code ---

/**
 * @brief Enum defining how a widget should be updated by an observer.
 */
typedef enum {
    OBSERVER_TYPE_LABEL_TEXT,
    OBSERVER_TYPE_SWITCH_STATE,
    OBSERVER_TYPE_SLIDER_VALUE,
    // Add more widget types here
} observer_update_type_t;

/**
 * @brief Adds a widget to the list of observers for a given state.
 * This is called by the generated create_ui() function.
 * @param state_name The state variable to observe.
 * @param widget The LVGL widget that will be updated.
 * @param update_type How the widget should be updated (e.g., change text, change state).
 * @param format An optional format string (e.g., "%.2f") for string-based updates.
 */
void data_binding_add_observer(const char* state_name, lv_obj_t* widget, observer_update_type_t update_type, const char* format);

/**
 * @brief Attaches an action to a widget.
 * This is called by the generated create_ui() function.
 * @param widget The LVGL widget that will trigger the action.
 * @param action_name The name of the action to be dispatched.
 * @param type The type of action (trigger, toggle, cycle).
 * @param cycle_values An array of values for ACTION_TYPE_CYCLE.
 * @param cycle_value_count The number of elements in cycle_values.
 */
void data_binding_add_action(lv_obj_t* widget, const char* action_name, action_type_t type, const binding_value_t* cycle_values, uint32_t cycle_value_count);


#endif // DATA_BINDING_H
