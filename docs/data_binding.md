# Data Binding: `observes` and `action`

The UI generator provides a powerful data binding system that creates a clean separation between your UI presentation (the YAML/JSON file) and your application logic (your C code). This system, based on a "Model-View-ViewModel" (MVVM) pattern, is managed by a small runtime library.

*   **View:** The UI definition in your YAML file.
*   **Model:** Your application's internal state (e.g., sensor values, machine status).
*   **ViewModel:** The `data_binding` library, which mediates between the Model and the View.

This system is comprised of two main features, configured by the `observes` and `action` keys in your UI definition.

## `action`: View-to-Model Communication

The `action` key allows widgets to send events and data to your application logic. When a user interacts with a widget (e.g., clicks a button, toggles a switch), a named action is dispatched to a single, centralized handler function in your C code.

### Action Types

There are three types of actions:

1.  **`trigger`**: A simple, stateless event. Ideal for buttons that perform a one-off task.
2.  **`toggle`**: Sends a boolean value representing the new state of a widget. Ideal for switches, and checkboxes. The event is triggered on value change.
3.  **`cycle`**: Cycles through a predefined list of values each time the widget is clicked. The new value is sent with the action. This is useful for buttons that select from a set of options (e.g., "50%", "100%", "150%").

### YAML Syntax

```yaml
# A simple trigger action
- type: button
  action: { program|run: trigger }

# A toggle action on a switch
- type: switch
  action: { spindle|toggle: toggle }

# A cycle action on a button. All numeric values are sent as floats.
- type: button
  action: { feedrate|override: [50, 90, 100, 110, 150] }
```

*   `program|run`, `spindle|toggle`, `feedrate|override` are the **action names**. You can define any string you like; they are how you identify the action in your C code.
*   `trigger`, `toggle`, or an array `[...]` define the **action type**.

### Implementing the Action Handler

To receive these actions in your application, you must implement a handler function and register it.

1.  **Define the Handler Function:** The function must match the `data_binding_action_handler_t` signature from `data_binding.h`.

    ```c
    // In your application C file (e.g., my_app.c)
    #include "data_binding.h"
    #include <stdio.h>

    void my_action_handler(const char* action_name, binding_value_t value) {
        printf("ACTION: name='%s' | type=%d\n", action_name, value.type);

        if (strcmp(action_name, "program|run") == 0) {
            // value.type is BINDING_TYPE_NULL for trigger actions
            start_program();
        }
        else if (strcmp(action_name, "spindle|toggle") == 0) {
            // value.type is BINDING_TYPE_BOOL for toggle actions
            bool is_on = value.as.b_val;
            set_spindle_state(is_on);
        }
        else if (strcmp(action_name, "feedrate|override") == 0) {
            // value.type is BINDING_TYPE_FLOAT for all numeric cycle actions
            float feedrate = value.as.f_val;
            set_feedrate_override(feedrate);
        }
    }
    ```

2.  **Register the Handler:** In your application's initialization routine, register this function with the data binding system. This only needs to be done once.

    ```c
    // In your application's init function
    #include "data_binding.h"

    void my_app_init(void) {
        // ... other initializations
        data_binding_register_action_handler(my_action_handler);
        // ...
    }
    ```

---

## `observes`: Model-to-View Communication

The `observes` key allows widgets to automatically update their appearance and state in response to changes in your application's model. You simply notify the data binding system when a piece of your data changes, and it handles the rest.

### Binding Targets

You can bind an observable state variable to one or more properties of a widget.

*   **`text`**: Updates the widget's text (e.g., for a `label`). The value is a `printf`-style format string.
*   **`visible`**: Toggles the widget's visibility (`LV_OBJ_FLAG_HIDDEN`).
*   **`checked`**: Toggles the widget's checked state (`LV_STATE_CHECKED`).
*   **`disabled`**: Toggles the widget's disabled state (`LV_STATE_DISABLED`).
*   **`style`**: Adds or removes a style from the widget.

### YAML Syntax

The `observes` key contains a map of `state_name` keys to binding definitions.

```yaml
- type: label
  id: '@status_label'
  observes:
    # State name to observe
    program|status:
      # Binding target: 'text'
      text: "Status: %s"
      # Binding target: 'style'
      style:
        'IDLE': '@style_idle'
        'RUNNING': '@style_running'
        'ERROR': '@style_error'
        default: null # If no match, remove all of the above styles

- type: button
  id: '@jog_button'
  observes:
    program|is_running: # Assume this is a boolean observable
      # Simple boolean mapping. If program|is_running is true,
      # the button will be disabled.
      disabled: true
      # You can also use an inverse mapping:
      # visible: false
      # This would mean: if program|is_running is true, the button is HIDDEN.
```

#### Map-Based Bindings

For `style`, `visible`, `checked`, and `disabled`, you can provide a map to handle non-boolean or multi-valued observables. The observable's value is used as a key to look up the desired UI property value.

*   The map keys can be strings, booleans (`true`/`false`), or numbers. All numeric keys are treated as floats.
*   A special `default` key can be provided as a fallback.
*   For `style` maps, a `null` value means "remove this mapped style", allowing a widget to return to its base style.

### Notifying the UI of State Changes

From your C application logic, whenever a piece of state changes, call `data_binding_notify_state_changed()`.

```c
// In your application C file (e.g., my_app.c)
#include "data_binding.h"

// Your application's state
typedef struct {
    bool program_is_running;
    const char* status_message;
} AppState;

AppState g_app_state;

void start_program() {
    g_app_state.program_is_running = true;
    g_app_state.status_message = "RUNNING";

    // Notify the UI of the changes. Any widgets observing these states will update.
    data_binding_notify_state_changed("program|is_running", (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = g_app_state.program_is_running});
    data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = g_app_state.status_message});
}

// In your app's main loop or a timer
void app_tick() {
    // ... logic that might change state
    float new_temp = read_sensor();

    // Notify a float value
    data_binding_notify_state_changed("sensor|temp", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = new_temp});
}
```

The `binding_value_t` union is used to pass data of different types (`float`, `bool`, `string`) into the data binding system. The system takes care of formatting it for `text` bindings or using it for lookups in map-based bindings.
