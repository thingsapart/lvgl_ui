# Data Binding in `lvgl_ui_generator`

The data binding system provides a powerful mechanism to connect the state of your application to the UI, and to receive events from the UI back into your application. It consists of two primary mechanisms: **Actions** (UI to App) and **Observers** (App to UI).

This system is designed to decouple your application logic from the UI creation code, allowing you to change the UI definition in YAML/JSON without needing to recompile your main application logic.

## Actions: UI to Application Communication

Actions are events triggered by widgets that your application can listen to. This is how you handle button clicks, switch toggles, slider value changes, etc.

### Defining Actions in YAML/JSON

You define an action on a widget using the `action` key. The value is a map where each key is a unique `action_name` string and the value specifies the `action_type`.

```yaml
- type: button
  action: { program|run: "trigger" } # A simple button click

- type: switch
  action: { spindle|toggle: "toggle" } # A switch/checkbox toggle

- type: button
  action: { feedrate|override: [50, 90, 100, 110, 150] } # A cycle button
```

**Action Types:**

*   **`trigger`**: A simple, stateless event. It's ideal for momentary actions like a standard button press. The value sent to the handler is of type `BINDING_TYPE_NULL`.
*   **`toggle`**: Used for widgets with an on/off state, like a `switch` or a `checkbox`. It sends the widget's new checked state (`true` or `false`) to the handler. The value is of type `BINDING_TYPE_BOOL`.
*   **`[value1, value2, ...]` (Cycle)**: An array of values attached to a widget (typically a button). Each time the action is triggered, it sends the *next* value in the array to the handler, cycling back to the start at the end. The value sent can be a float, bool, or string.

### Implementing an Action Handler

To receive these actions, your application must provide a single callback function, the **Action Handler**.

1.  **Define the Handler Function**: The function must match the `data_binding_action_handler_t` signature defined in `data_binding.h`.

    ```c
    #include "data_binding.h"
    #include <stdio.h>

    // The single action handler for the entire application.
    void my_app_action_handler(const char* action_name, binding_value_t value) {
        printf("Action received: '%s'\n", action_name);

        if (strcmp(action_name, "program|run") == 0) {
            // Handle the 'run' action. The value is NULL for a 'trigger'.
            start_program();
        }
        else if (strcmp(action_name, "spindle|toggle") == 0) {
            // Handle the spindle toggle. The value is a boolean.
            bool is_on = value.as.b_val;
            set_spindle_state(is_on);
        }
        else if (strcmp(action_name, "feedrate|override") == 0) {
            // Handle the feedrate cycle button. The value is a float.
            float new_feedrate = value.as.f_val;
            set_feedrate_override(new_feedrate);
        }
    }
    ```

2.  **Register the Handler**: In your application's initialization code, you must register this handler with the data binding system. This is typically done once at startup.

    ```c
    #include "data_binding.h"

    void my_app_init(void) {
        // ... other initializations ...

        data_binding_init(); // Initialize the binding system
        data_binding_register_action_handler(my_app_action_handler);

        // ... rest of initialization ...
    }
    ```

Now, whenever a widget with an `action` is interacted with, `my_app_action_handler` will be called with the corresponding `action_name` and value.

## Observers: Application to UI Communication

Observers allow widgets to automatically update their appearance or state in response to changes in your application's state. You define what a widget "observes", and when your application notifies a change to that state, the UI updates itself.

### Defining Observers in YAML/JSON

Observers are defined using the `observes` key on a widget. The value is a map where each key is a `state_name` string from your application (e.g., `program|status`), and the value is another map defining one or more **bindings**.

A binding consists of a *binding type* (e.g., `text`, `style`) and its *configuration*. A single widget can observe multiple states, and a single state can trigger multiple bindings on a widget.

```yaml
- type: label
  id: "@status_label"
  text: "STATUS: IDLE" # Initial text
  observes:
    # This widget observes the "program|status" state
    program|status:
      # Binding 1: Update the text
      text: "STATUS: %s"
      # Binding 2: Update the style based on the state's value
      style:
        RUNNING: '@status_running'
        default: '@status_idle'

- type: button
  observes:
    program|is_running:
      # This map controls the visibility flag.
      visible:
        true: false  # When is_running is true, button is NOT visible
        false: true  # When is_running is false, button IS visible
    program|status:
      # This map controls the disabled state.
      disabled:
        RUNNING: true   # When status is "RUNNING", button is disabled
        default: false # For any other status, it's enabled
```

### Observer Binding Types

*   **`text`**: Updates the widget's text. The value must be a `printf`-style format string. The incoming state value will be used as the argument.
    ```yaml
    observes: { position|x: { text: "X: %.2f" } }
    ```

*   **`style`**: Applies a style to the widget based on the state's value. The value must be a map where keys are possible state values and values are `@style_id` references.
    *   The special value `null` can be used to remove the style applied by this binding.
    *   A special key `"default"` provides a fallback style.
    *   Keys in the map can be strings (`RUNNING`), numbers (`1.0`), or booleans (`true`).
    ```yaml
    observes:
      jog|step:
        style:
          1.0: '@style_btn_active'
          10.0: '@style_btn_active'
          default: null
    ```

*   **`visible`**: Toggles the widget's visibility (`LV_OBJ_FLAG_HIDDEN`). The value is a map from state values to booleans (`true` for visible, `false` for hidden).

*   **`checked`**: Toggles the widget's checked state (`LV_STATE_CHECKED`). The value is a map from state values to booleans. Useful for `switch` widgets or making buttons appear active.

*   **`disabled`**: Toggles the widget's disabled state (`LV_STATE_DISABLED`). The value is a map from state values to booleans.

### Notifying the UI of State Changes

To trigger these UI updates, your application must call `data_binding_notify_state_changed` whenever a piece of its state changes.

```c
#include "data_binding.h"

// Example state in your application
typedef struct {
    bool is_running;
    const char* status_message;
} AppState;

AppState g_app_state;

void start_program() {
    g_app_state.is_running = true;
    g_app_state.status_message = "RUNNING";

    // Notify the UI that these states have changed
    data_binding_notify_state_changed(
        "program|is_running",
        (binding_value_t){ .type = BINDING_TYPE_BOOL, .as.b_val = g_app_state.is_running }
    );
    data_binding_notify_state_changed(
        "program|status",
        (binding_value_t){ .type = BINDING_TYPE_STRING, .as.s_val = g_app_state.status_message }
    );
}
```

The `binding_value_t` struct is a tagged union used to pass data of different types (`float`, `bool`, `string`) into the binding system. When you call `notify_state_changed`, the data binding library finds all widgets observing that `state_name` and updates them according to the rules you defined in the YAML/JSON.

For a comprehensive set of examples, see the **`ex_cnc/cnc_ui.yml`** file provided with the generator.
