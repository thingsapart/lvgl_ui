# Data Bindings: Observables/Actions

Based on a self-contained runtime library called **`data_binding`**. The generator will be modified to parse the new `observes` and `action` keys and will emit C code that calls functions from this library. This keeps the generated code clean and centralizes the complex binding and event logic.

1.  **Core Components:**
    *   **`data_binding.h` / `data_binding.c`:** A new, reusable C library that the final application links against. It manages observer lists and action callbacks. It is the "ViewModel" in this architecture.
    *   **`binding_value_t`:** A new union type to pass typed data (`int`, `float`, `bool`, `string`) between the application model and the data binding library.
    *   **Generator (`generator.c`):** Updated to recognize `observes` and `action` keys. It will not generate complex logic, but rather simple calls to the `data_binding` library's API.

2.  **Data Flow (Model -> View):**
    *   **Declarative:** `my_label: { observes: { 'state::name': { float: "Value: %.2f" } } }`
    *   **Generation:** The generator outputs a C call: `data_binding_add_observer("state::name", my_label_widget, OBSERVER_TYPE_LABEL_TEXT, "Value: %.2f");`
    *   **Runtime:** The application's model calls `data_binding_notify_state_changed("state::name", (binding_value_t){.f_val = 123.45});`. The library finds all registered observers for that name and updates their LVGL properties (e.g., `lv_label_set_text`).

3.  **Data Flow (View -> Model):**
    *   **Declarative:** `my_button: { action: { 'action::name': 'trigger' } }` or `my_cycle_button: { action: { 'action::cycle': [10, 20, 30] } }`
    *   **Generation:** The generator outputs a call to a setup function: `data_binding_add_action(my_button_widget, "action::name", ACTION_TYPE_TRIGGER, NULL);`
    *   **Runtime:** The `data_binding` library attaches a generic LVGL event callback to the widget. When the user interacts (e.g., clicks), the callback fires. It reads its local state from `user_data` if necessary (e.g., for cycling), and then calls the single, user-registered action handler: `my_action_handler("action::name", new_value);`.
