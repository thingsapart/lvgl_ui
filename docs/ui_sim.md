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
      target_state: { modifier: arguments }

updates:
  # On every tick, run this modification.
  - target_state: { modifier: arguments, when: [condition] }
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
| **`set`** | `value_expression` | Sets the state variable to the result of the expression. |
| **`inc`** | `numeric_expression` | Increments a numeric state by the result of the expression. |
| **`dec`** | `numeric_expression` | Decrements a numeric state by the result of the expression. |
| **`toggle`** | (none) | Flips a boolean state variable from `true` to `false` or vice-versa. |
| **`cycle`** | `[val1, val2, ...]` | Cycles through a list of values. Sets the state to the next value on each execution. |
| **`range`** | `[min, max, step]` | A cyclic increment. Increments the state by `step`. If the result exceeds `max`, it wraps to `min`. |

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
| **Not Equals** | `[!=, A, B]` | True if A is not equal to B. |
| **Comparison** | `[>]`, `[<]`, `[>=]`, `[<=]` | Numeric comparison. |
| **Logic** | `[and, C1, C2]`, `[or, C1, C2]`, `[not, C]` | Logical operations on conditions. |

### The `case` Expression

This provides `if/else if/else` functionality. It evaluates conditions top-to-bottom and returns the value associated with the first true condition.

**Syntax:** `{ case: [[condition1, value1], [condition2, value2], [true, else_value]] }`

```yaml
# Example: Determine a status string
program_status:
  set:
    case:
      - [program_running, "RUNNING"]
      - [spindle_on, "SPINDLE ON"]
      - [true, "IDLE"] # The 'else' case
```

---

## Example Use-Cases

### 1. Simple Thermostat

*   **Goal:** Adjust a target temperature with +/- buttons. A simulated current temperature slowly "moves" towards the target.
*   **Concepts:** `inc`, `dec`, `updates`, conditional movement.

```yaml
- type: data-binding
  state:
    - current_temp: 22.0
    - target_temp: 22.0
    - heater_on: false
  actions:
    - temp_up:
        inc: { target_temp: 0.5 }
    - temp_down:
        dec: { target_temp: 0.5 }
  updates:
    # Simulate current temperature moving towards target
    - inc: { current_temp: 0.1, when: [<, current_temp, target_temp] }
    - dec: { current_temp: 0.1, when: [>, current_temp, target_temp] }
    # Set heater status based on temperature diff
    - set:
        heater_on:
          case:
            - [[>, [sub, target_temp, current_temp], 0.2], true]
            - [true, false]
```

### 2. CNC Machine Control Panel

*   **Goal:** A simplified version of the original CNC example, demonstrating state transitions and disabling controls.
*   **Concepts:** `case`, `set`, boolean state transitions.

```yaml
- type: data-binding
  state:
    - time: 0.0
    - program_running: false
    - spindle_on: false
    - pos_x: 0.0
    - pos_y: 0.0
    - spindle_rpm: 0.0
    - program_status:
        derived_expr:
          case:
            - [program_running, "RUNNING"]
            - [spindle_on, "SPINDLE ON"]
            - [true, "IDLE"]
  actions:
    - program_run:
        set: { program_running: true, spindle_on: true }
    - program_stop:
        set: { program_running: false, spindle_on: false }
  updates:
    - inc: { time: 0.033 }
    - set:
        spindle_rpm:
          case:
            - [spindle_on, [add, 8000, [mul, 200, [sin, [mul, time, 10]]]]]
            - [true, 0]
    - when:
        condition: program_running
        then:
          - set: { pos_x: [mul, 50, [cos, time]] }
          - set: { pos_y: [mul, 50, [sin, time]] }
```

### 3. 3D Printer Progress Screen

*   **Goal:** Show an animated print progress bar and estimated time remaining.
*   **Concepts:** `inc`, `clamp`, `div`, simulating progress.

```yaml
- type: data-binding
  state:
    - printing: false
    - progress_pct: 0.0
    - time_elapsed: 0.0
    - time_total: 3600.0 # 1 hour total print time
    - time_remaining: 3600.0
  actions:
    - start_print:
        set: { printing: true, progress_pct: 0.0, time_elapsed: 0.0 }
    - cancel_print:
        set: { printing: false }
  updates:
    - when:
        condition: printing
        then:
          - inc: { time_elapsed: 1 } # Assume 1 tick = 1 second
          - set:
              progress_pct:
                [clamp, [mul, [div, time_elapsed, time_total], 100], 0, 100]
          - set: { time_remaining: [sub, time_total, time_elapsed] }
          - when:
              condition: [>=, progress_pct, 100]
              then:
                - set: { printing: false }
```

### 4. Audio Mixer Waveform Selector

*   **Goal:** A button that cycles through different synthesizer waveforms.
*   **Concepts:** `cycle` modifier for strings.

```yaml
- type: data-binding
  state:
    - waveform: [string, "SINE"]
    - frequency: 440.0
    - amplitude: 0.8
  actions:
    - cycle_waveform:
        cycle: { waveform: [cycle, ["SINE", "SQUARE", "SAW", "TRIANGLE"]] }
    - set_frequency: # Assumes a slider sends value.float
        set: { frequency: value.float }
```

### 5. Medical Device ECG Waveform

*   **Goal:** Drive a chart with a simulated, repeating ECG-like pattern.
*   **Concepts:** `sin`, `cos`, `time`, multiple `when` clauses for a complex waveform.

```yaml
- type: data-binding
  state:
    - time: 0.0
    - ecg_val: 0.0
    - display_frozen: false
  actions:
    - toggle_freeze:
        toggle: { display_frozen: true }
  updates:
    - when:
        condition: !display_frozen
        then:
          # Simple P-QRS-T approximation
          - inc: { time: 0.05 }
          - set:
              ecg_val:
                add:
                  - [mul, 0.1, [sin, [mul, time, 3.14]]]  # P wave
                  - [mul, 1.5, [sin, [mul, [sub, time, 0.5], 30]]] # QRS complex
                  - [mul, 0.3, [sin, [mul, [sub, time, 0.9], 4]]]   # T wave
          # Reset time to make it loop
          - when:
              condition: [>, time, 6.28]
              then:
                - set: { time: 0.0 }
```

### 6. Smart Home Light Control

*   **Goal:** A simple light switch and a brightness slider.
*   **Concepts:** `toggle`, `clamp`.

```yaml
- type: data-binding
  state:
    - light_on: false
    - brightness: 100.0 # As a percentage
  actions:
    - toggle_light:
        toggle: { light_on: true }
    - set_brightness:
        set: { brightness: [clamp, value.float, 0, 100] }
```

### 7. EV Charger Interface

*   **Goal:** Simulate the charging state machine of an EV charger.
*   **Concepts:** `case`, state transitions, simulating energy delivery.

```yaml
- type: data-binding
  state:
    - status: [string, "Available"]
    - energy_delivered: 0.0
    - charge_rate_kw: 0.0
    - battery_pct: 35.0
  actions:
    - plug_in:
        when:
          condition: [==, status, "Available"]
          then:
            - set: { status: "Plugged In", charge_rate_kw: 0.0 }
    - start_charge:
        when:
          condition: [==, status, "Plugged In"]
          then:
            - set: { status: "Charging", charge_rate_kw: 11.0 }
    - stop_charge:
        when:
          condition: [==, status, "Charging"]
          then:
            - set: { status: "Finished", charge_rate_kw: 0.0 }
  updates:
    - when:
        condition: [==, status, "Charging"]
        then:
          # Assuming 1 tick = 1 second, kW * sec / 3600 = kWh
          - inc: { energy_delivered: [div, charge_rate_kw, 3600] }
          # A very rough battery percentage simulation
          - inc: { battery_pct: [div, charge_rate_kw, 1000] }
          - when:
              condition: [>=, battery_pct, 100]
              then:
                - set: { status: "Finished", charge_rate_kw: 0.0, battery_pct: 100.0 }
```

### 8. Industrial Robot Safety Interlock

*   **Goal:** A safety key switch that enables/disables jog controls.
*   **Concepts:** Boolean logic, using one state to gate another's `when` clause.

```yaml
- type: data-binding
  state:
    - safety_key_on: false
    - x_pos: 100.0
    - jog_speed: 10.0
  actions:
    - key_turned:
        toggle: { safety_key_on: true }
    - jog_x_plus:
        when:
          # Both conditions must be true to execute
          condition: [and, safety_key_on, [>, x_pos, 0]]
          then:
            - dec: { x_pos: jog_speed }
```

### 9. Coffee Machine Wizard

*   **Goal:** A multi-step setup screen controlled by a "Next" button.
*   **Concepts:** Using a numeric state as a step counter to control UI visibility.

```yaml
- type: data-binding
  state:
    - setup_step: 0 # 0=Welcome, 1=Strength, 2=Size, 3=Confirm
  actions:
    - next_step:
        inc: { setup_step: 1 }
    - prev_step:
        dec: { setup_step: 1 }
    - finish_setup:
        set: { setup_step: 0 } # Reset after completion
  updates:
    # Clamp step to valid range
    - set: { setup_step: [clamp, setup_step, 0, 3] }
```

### 10. Simple Game UI

*   **Goal:** A character health bar that can be modified by actions.
*   **Concepts:** `inc`, `dec`, and `clamp` working together.

```yaml
- type: data-binding
  state:
    - health: 100.0
    - mana: 50.0
  actions:
    - take_damage:
        set: { health: [clamp, [sub, health, 20], 0, 100] }
    - use_potion:
        set: { health: [clamp, [add, health, 50], 0, 100] }
    - cast_spell:
        when:
          condition: [>=, mana, 10]
          then:
            - dec: { mana: 10 }
