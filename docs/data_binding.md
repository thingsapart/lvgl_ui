# Data Binding - "action" and "observes"

The data binding system provides a mechanism to connect the state of your application to the UI, and to receive events from the UI back into your application logic. It is based on a "Model-View-ViewModel" (MVVM) pattern that creates a clean separation of concerns:

*   **View:** The UI definition in your YAML file.
*   **Model:** Your application's internal state (e.g., sensor values, machine status).
*   **ViewModel:** The `data_binding` library, which mediates between the Model and the View.

This system is comprised of two main features, configured by the `action` and `observes` keys in your UI definition.

## `action`: UI to Application Communication

The `action` key allows widgets to send events and data to your application logic. When a user interacts with a widget (e.g., clicks a button, toggles a switch), a named action is dispatched to a single, centralized handler function in your C code.

### Defining Actions in YAML

You define an action on a widget using the `action` key. The value is a map where each key is a unique `action_name` string and the value specifies the `action_type`.

```yaml
# A simple trigger action
- type: button
  action: { program|run: "trigger" }

# A toggle action on a switch
- type: switch
  action: { spindle|toggle: "toggle" }

# A cycle action on a button. All numeric values are sent as floats.
- type: button
  action: { feedrate|override: [50, 90, 100, 110, 150] }
```

*   `program|run`, `spindle|toggle`, and `feedrate|override` are the **action names**. You can define any string; they are how you identify the action in your C code.
*   The value (`"trigger"`, `"toggle"`, or an array `[...]`) defines the **action type**.

### Action Types

1.  **`trigger`**: A simple, stateless event. Ideal for buttons that perform a one-off task. The `binding_value_t` sent to the handler has type `BINDING_TYPE_NULL`.
2.  **`toggle`**: Sends a boolean value representing the new state of a widget. Ideal for `switch` and `checkbox` widgets. The event is triggered on value change, and the value has type `BINDING_TYPE_BOOL`.
3.  **`cycle` (`[...]`)**: An array of values attached to a widget. Each time the action is triggered, it sends the *next* value in the array to the handler, cycling back to the start. The value sent can be a float, bool, or string.

### Implementing the Action Handler in C

To receive these actions in your application, you must implement a single handler function and register it.

1.  **Define the Handler Function:** The function must match the `data_binding_action_handler_t` signature from `data_binding.h`.

    ```c
    // In your application C file (e.g., my_app.c)
    #include "data_binding.h"
    #include <stdio.h>

    void my_action_handler(const char* action_name, binding_value_t value) {
        printf("ACTION: name='%s', type=%d\n", action_name, value.type);

        if (strcmp(action_name, "program|run") == 0) {
            // value.type is BINDING_TYPE_NULL for a trigger
            start_program();
        }
        else if (strcmp(action_name, "spindle|toggle") == 0) {
            // value.type is BINDING_TYPE_BOOL for a toggle
            bool is_on = value.as.b_val;
            set_spindle_state(is_on);
        }
        else if (strcmp(action_name, "feedrate|override") == 0) {
            // value.type is BINDING_TYPE_FLOAT for numeric cycle values
            float feedrate = value.as.f_val;
            set_feedrate_override(feedrate);
        }
    }
    ```

2.  **Register the Handler:** In your application's initialization routine, register this function. This only needs to be done once.

    ```c
    // In your application's init function
    #include "data_binding.h"

    void my_app_init(void) {
        // ... other initializations
        data_binding_init(); // Must be called first
        data_binding_register_action_handler(my_action_handler);
        // ...
    }
    ```

---

## `observes`: Application to UI Communication

The `observes` key allows widgets to automatically update their appearance in response to changes in your application's data model. You simply notify the data binding system when a piece of your data changes, and it handles updating the UI.

### Defining Observers in YAML

The `observes` key contains a map of `state_name` keys to one or more **bindings**. A single widget can observe multiple states, and a single state can trigger multiple bindings on a widget.

```yaml
- type: label
  id: '@status_label'
  observes:
    # State name to observe
    program|status:
      # Binding 1: update the 'text' property
      text: "Status: %s"
      # Binding 2: update the 'style' property
      style:
        'IDLE': '@style_idle'
        'RUNNING': '@style_running'
        'ERROR': '@style_error'
        default: null # If no match, remove any styles previously applied by this map

- type: button
  id: '@jog_button'
  observes:
    # Observe one state to control visibility
    program|mode:
      visible:
        'MANUAL': true
        'AUTO': false
        default: true
    # Observe another state to control the disabled property
    program|is_running:
      disabled:
        true: true
        false: false
```

### Observer Binding Types

*   **`text`**: Updates the widget's text (e.g., for a `label`). The value must be a `printf`-style format string.
    ```yaml
    observes: { position|x: { text: "X: %.2f" } }
    ```
*   **`style`**: Applies a style to the widget based on the state's value. The configuration must be a map where keys are possible state values and values are `@style_id` references.
    *   The map keys can be strings (`'IDLE'`), numbers (`1.0`), or booleans (`true`).
    *   A special key `"default"` provides a fallback style.
    *   A `null` value means "remove the style previously applied by this binding," reverting the widget to its base style for that state.
    ```yaml
    observes:
      jog|step:
        style:
          1.0: '@style_btn_active'
          10.0: '@style_btn_active'
          default: null
    ```
*   **`visible`**: Toggles the widget's visibility (`LV_OBJ_FLAG_HIDDEN`). The configuration must be a map from state values to booleans (`true` for visible, `false` for hidden).
*   **`checked`**: Toggles the widget's checked state (`LV_STATE_CHECKED`). The configuration must be a map from state values to booleans.
*   **`disabled`**: Toggles the widget's disabled state (`LV_STATE_DISABLED`). The configuration must be a map from state values to booleans.

### Notifying the UI of State Changes in C

From your application logic, whenever a piece of state changes, call `data_binding_notify_state_changed()`.

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
    data_binding_notify_state_changed("program|is_running",
        (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = g_app_state.program_is_running});

    data_binding_notify_state_changed("program|status",
        (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = g_app_state.status_message});
}

// In your app's main loop or a timer
void app_tick() {
    // ... logic that might change state
    float new_temp = read_sensor();

    // Notify a float value
    data_binding_notify_state_changed("sensor|temp",
        (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = new_temp});
}
```

The `binding_value_t` union is used to pass data of different types (`float`, `bool`, `string`) into the data binding system. The system takes care of formatting it for `text` bindings or using it for lookups in map-based bindings.

For a comprehensive set of examples, see the **`ex_cnc/cnc_ui.yml`** file provided with the generator.
