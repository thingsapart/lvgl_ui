# UI-Sim: A Declarative UI Simulator

UI-Sim is a powerful feature that allows you to create a fully interactive, stateful UI prototype without writing any C application code. It acts as a "mock backend" or a "state engine" that you define entirely within your YAML/JSON UI specification.

This enables a rapid design-and-test workflow where a UI designer can build and validate complex user interactions, animations, and state transitions directly in the live preview, long before the real application logic is available.

## Core Concepts

The system is controlled by a single `type: data-binding` block in your UI spec. This block defines four key things:

1.  **`state`**: The data model of your mock application. This is a collection of named variables (e.g., `temperature`, `is_running`, `status_message`) that represent the state of your simulated device.
2.  **`actions`**: The logic that runs when a UI widget triggers an action (e.g., a button is clicked). Actions modify the `state`.
3.  **`updates`**: The logic that runs periodically on a timer (about 30 times per second). This is used to simulate continuous processes, like a changing sensor value, a running motor, or an animated progress bar.
4.  **`schedule`**: (For Testing) A list of actions to be triggered automatically at specific ticks. This is essential for creating automated, reproducible tests of your action logic.

When any variable in the `state` is changed (either by an `action`, an `update`, or the `schedule`), UI-Sim automatically notifies the data binding system, and any widget observing that variable will update itself.

---

## Syntax Reference

### The `data-binding` Block

This is the root object for the UI-Sim definition. There should only be one in a UI file.

```yaml
- type: data-binding
  state:
    # ... state variable declarations ...
  actions:
    # ... action handlers ...
  updates:
    # ... periodic update rules ...
  schedule:
    # ... scheduled test actions ...
```

### The `state` Block

This is a list where each item declares a state variable.

**Syntax:** `- <variable_name>: <type_and_initial_value>`

| Format | Description | Example |
| :--- | :--- | :--- |
| **Explicit Type & Value** | The most robust format. The array contains `[type, initial_value]`. Valid types are `"float"`, `"bool"`, and `"string"`. | `- temperature: [float, 25.5]` |
| **Inferred Type** | Provide a literal initial value. The type is inferred (`25.0` -> float, `true` -> bool, `"text"` -> string). | `- is_on: false` |
| **Explicit Type Only** | Provide only the type name. A default initial value is used (0.0, false, ""). | `- status: string` |
| **Derived Expression** | Creates a read-only variable whose value is calculated from an expression. See the Expression Language section. | `- status: { derived_expr: ... }` |

### The `actions` and `updates` Blocks

Both blocks use the same syntax: a list of modification rules. For `actions`, the rule is wrapped in the action name.

```yaml
actions:
  # When the UI dispatches an action named "my_action", run this modification.
  - my_action:
      modifier: arguments

updates:
  # On every tick, run this modification.
  - modifier: { target_state: arguments, when: [condition] }
```

### The `schedule` Block (For Testing)

This block defines a series of actions that will be automatically executed by the test runner (`--run-sim-test`).

**Syntax:** a list of scheduled action objects.

```yaml
schedule:
  - { tick: <tick_number>, action: "<action_name>", with: <value> }
```

*   `tick`: The 1-based tick number on which to trigger the action.
*   `action`: The name of the action to trigger (must be defined in the `actions` block).
*   `with`: (Optional) The payload value to send with the action. This corresponds to `value.float`, `value.bool`, etc., inside the action's expression logic.

**Example:**
```yaml
schedule:
  - { tick: 2, action: "start_machine" }
  - { tick: 5, action: "set_speed", with: 120.5 }
```

### Modification Reference

A modification is an operation that changes a state variable.

| Modifier | Argument(s) | Description |
| :--- | :--- | :--- |
| **`set`** | `{ <state>: <expr> }` | Sets the state variable to the result of the expression. |
| **`inc`** | `{ <state>: <expr> }` | Increments a numeric state by the result of the expression. |
| **`dec`** | `{ <state>: <expr> }` | Decrements a numeric state by the result of the expression. |
| **`toggle`** | `<state_name_string>` | Flips a boolean state variable from `true` to `false` or vice-versa. |
| **`cycle`** | `{ <state>: [val1, ...]}` | Cycles through a list of values. Sets the state to the next value on each execution. |
| **`range`** | `{ <state>: [min, max, step]}` | A cyclic increment. Increments the state by `step`. If it exceeds `max` or `min`, it wraps. |

---

## The Expression Language

UI-Sim uses a simple but powerful expression language based on LISP-style "S-expressions" (Symbolic Expressions). An expression is either a literal value or a list where the first item is a function/operator.

### Literals and References

| Type | Syntax | Description |
| :--- | :--- | :--- |
| **Number** | `123.4` | Always treated as a float. |
| **Boolean** | `true` or `false` | Unquoted boolean literals. |
| **String** | `"some text"` | A string literal. |
| **State Reference** | `state_name` | The current value of another state variable. |
| **Negated Boolean** | `!state_name` | The negated value of a boolean state variable. |
| **Action Payload** | `value.float` | The incoming value from a UI action (e.g., a toggle switch). Use `value.bool` or `value.string` for other types. |
| **Time** | `time` | A special read-only variable managed by the simulator. It increments by a small amount each tick and can be used for animations and time-based logic. |

### Functions and Operators

**Arithmetic:**
`[add, A, B]`, `[sub, A, B]`, `[mul, A, B]`, `[div, A, B]`

**Trigonometry:**
`[sin, A]`, `[cos, A]` (A is in radians)

**Clamping:**
`[clamp, value, min, max]`

**Conditionals:**

| Operator | Syntax | Description |
| :--- | :--- | :--- |
| **Equals** | `[==, A, B]` | True if A equals B. |
| **Not Equals** | `[!=, A,
