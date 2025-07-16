#ifndef DATA_BINDING_H
#define DATA_BINDING_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

// --- Public Data Types ---

/**
 * @brief Enum for the types of values that can be passed through the binding system.
 * All numeric types are consolidated into BINDING_TYPE_FLOAT.
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
} action_type_t;

/**
 * @brief Enum defining how a widget should be updated by an observer.
 */
typedef enum {
    OBSERVER_TYPE_TEXT,
    OBSERVER_TYPE_STYLE,
    OBSERVER_TYPE_VISIBLE,
    OBSERVER_TYPE_CHECKED,
    OBSERVER_TYPE_DISABLED,
    OBSERVER_TYPE_VALUE, // For sliders, etc.
} observer_update_type_t;


/**
 * @brief A function pointer for the application's main action handler.
 * This single function will receive all actions triggered by the UI.
 * @param action_name The name of the action being triggered.
 * @param value The value associated with the action (if any).
 * @param user_data The user-provided context pointer registered at initialization.
 */
typedef void (*data_binding_action_handler_t)(const char* action_name, binding_value_t value, void* user_data);


// --- Public API for the Main Application ---

/**
 * @brief Initializes the data binding system. Must be called once.
 */
void data_binding_init(void);

/**
 * @brief Registers the application's single action handler function and a user context pointer.
 * @param handler A pointer to the function that will process UI actions.
 * @param user_data A pointer to an application-defined context struct or variable. This
 *                  pointer will be passed to every invocation of the action handler.
 */
void data_binding_register_action_handler(data_binding_action_handler_t handler, void* user_data);

/**
 * @brief Notifies the UI that a piece of the application's state has changed.
 * The data binding library will find all widgets observing this state and update them.
 * @param state_name The unique name of the state variable (e.g., "position::x").
 * @param new_value The new value of the state in a binding_value_t.
 */
void data_binding_notify_state_changed(const char* state_name, binding_value_t new_value);


// --- Internal API for Generated Code ---

// Generic map entry structure used by generated code.
// Note: The key is a binding_value_t to support string, bool, and numeric (float) keys.
typedef struct {
    binding_value_t key;
    // The value's interpretation depends on the observer type.
    // For STYLE, it's a pointer to an lv_style_t.
    // For VISIBLE/CHECKED/DISABLED, it's a bool.
    union {
        void* p_val;
        bool b_val;
    } value;
} binding_map_entry_t;

/**
 * @brief Adds a widget to the list of observers for a given state.
 * This is the generic function called by the generated create_ui() function.
 * @param state_name The state variable to observe.
 * @param widget The LVGL widget that will be updated.
 * @param update_type How the widget should be updated (e.g., change text, change style).
 * @param config A pointer to the configuration data. The type of this data depends on `update_type`:
 *        - OBSERVER_TYPE_TEXT: `const char*` (format string)
 *        - OBSERVER_TYPE_VALUE: `const char*` (format string, for float-to-int conversion)
 *        - OBSERVER_TYPE_VISIBLE/CHECKED/DISABLED (direct map): `const bool*` (true for direct, false for inverse)
 *        - OBSERVER_TYPE_STYLE/VISIBLE/CHECKED/DISABLED (map): `const binding_map_entry_t*` (array of map entries)
 * @param config_len For map-based observers, this is the number of entries in the map array.
 * @param default_value A pointer to a default value for map-based observers.
 */
void data_binding_add_observer(const char* state_name, lv_obj_t* widget,
                               observer_update_type_t update_type,
                               const void* config, size_t config_len, const void* default_value);

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
