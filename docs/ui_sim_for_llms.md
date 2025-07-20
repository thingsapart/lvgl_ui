# GENERATING UI-SIM `data-binding` BLOCKS WITH LLMS - AKA LLM INSTRUCTION MANUAL

LLMs are pretty good at writing complex UI-Sims. But remember it's best to keep things simpler-ish. Simply prepend your request with a short instruction like: *"Using the provided documentation, generate a `data-binding` block for the following scenario: [user's problem description]"*, and then paste the entire contents of this markdown document.

---

> PASTE the rest of this doc's source into your LLM chat...

---

## 1. YOUR OBJECTIVE

You are an expert in the `UI-Sim` declarative simulation engine. Your primary function is to take a user's description of an interactive device or UI and generate a complete, correct, and well-structured `data-binding` block in YAML format. This block will serve as a fully functional, interactive mock backend for a UI prototype.

You will adhere strictly to the syntax and simulation mechanics described in this document.

## 2. CORE CONCEPTS

The entire simulation is defined within a single top-level YAML object: `- type: data-binding`. This object contains up to four key sections:

1.  **`state`**: The data model. This is the complete set of variables that define the current state of the simulation (e.g., `temperature`, `is_running`). **Everything the UI needs to display or react to must be represented here.**
2.  **`actions`**: Event-driven logic. These are named blocks of code that execute *only* when triggered by a UI event (like a button press). Actions are the primary way user interaction modifies the `state`.
3.  **`updates`**: Continuous logic. This is a list of modifications that are evaluated and executed on every "tick" of the simulator (approximately 30 times per second). This is used for animations, timers, and simulating ongoing processes.
4.  **`schedule`**: Test automation. A list of actions to be triggered at specific ticks, used for creating reproducible tests. You will generally only generate this if the user is asking to create a test case.

**The Reactive Flow:**
`Action` or `Update` modifies `State` -> The change is automatically propagated to the UI -> UI widgets observing that state variable update themselves.

### **THE GOLDEN RULE: ONE BLOCK TO RULE THEM ALL**

**IMPORTANT:** The entire simulation logic—all state, actions, and updates—*must* be contained within a single `- type: data-binding` block. Do not create multiple blocks.

```yaml
# CORRECT STRUCTURE
- type: data-binding
  state:
    # ... all state variables ...
  actions:
    # ... all actions ...
  updates:
    # ... all updates ...

# INCORRECT STRUCTURE (DO NOT DO THIS)
- type: data-binding
  state:
    - var1: 10
- type: data-binding
  actions:
    - my_action: ...
```

---

## 3. SECTION-BY-SECTION SYNTAX

### 3.1. The `state` Block

This is a YAML list defining all variables in your simulation's data model.

| Declaration Format | Syntax & Description | Example |
| :--- | :--- | :--- |
| **Inferred Type** | `- <name>: <literal_value>` <br> The type (`float`, `bool`, `string`) is inferred from the value. | `- temperature: 22.5` <br> `- is_on: true` <br> `- status_msg: "Ready"` |
| **Explicit Type Only** | `- <name>: <type_name>` <br> Use a default value (0.0, false, ""). | `- pressure: float` <br> `- error_flag: bool` <br> `- user_name: string` |
| **Explicit Type & Value** | `- <name>: [<type_name>, <value>]` <br> The most explicit and robust format. | `- target_temp: [float, 25.0]` <br> `- door_open: [bool, false]` <br> `- wcs_name: [string, "G54"]` |
| **Derived Expression** | `- <name>: { derived_expr: <expression> }` <br> A read-only variable whose value is automatically calculated whenever its dependencies change. See Expression Language section. | `- temp_fahrenheit: { derived_expr: [add, [mul, temp_celsius, 1.8], 32] }` |

#### `state` Best Practices & Gotchas

*   **Be Explicit:** When in doubt, prefer the `[type, value]` format for maximum clarity.
*   **The `time` Variable:** A special, read-only `float` state variable named `time` is automatically provided. It starts at 0.0 and increments by a small amount each tick. It is essential for animations and time-based simulations. Do not declare it yourself.
*   **Derived State is Read-Only:** You cannot modify a variable with `derived_expr` using an action or update. It is *derived* from other states.

### 3.2. The `actions` & `updates` Blocks: Modifiers

Actions and updates are built from "modifiers," which are operations that change state variables.

| Modifier | Argument(s) | Description & Use Case |
| :--- | :--- | :--- |
| **`set`** | `{ <state>: <expression> }` | Sets a state variable to the result of an expression. The most common modifier. |
| **`inc`** | `{ <state>: <expression> }` | Increments a numeric state by the expression's result. For counters, step controls. |
| **`dec`** | `{ <state>: <expression> }` | Decrements a numeric state by the expression's result. |
| **`toggle`** | `<state_name_string>` | Flips a boolean state variable (`true` -> `false`, `false` -> `true`). For on/off switches. |
| **`cycle`** | `{ <state>: [val1, val2, ...]}` | Cycles through a list of values. Sets the state to the next value in the list on each execution. For mode selection. |
| **`range`** | `{ <state>: [min, max, step]}` | A cyclic increment/decrement. If `step` is positive and value > `max`, it wraps to `min`. If `step` is negative and value < `min`, it wraps to `max`. |

### 3.3. `actions` Block Deep Dive

The `actions` block is a list where each item is an object with a single key: the action's name.

```yaml
actions:
  # Action with a single modification
  - program_stop:
      set: { program_running: false, spindle_on: false }

  # Action with a conditional modification
  - cast_spell:
      when:
        condition: [>=, mana, 10]
        then:
          - dec: { mana: 10 }
```

#### `actions` Best Practices & Gotchas

*   **Action Payload:** Actions can receive data from the UI. This data is accessed via the special `value` literal in expressions: `value.float`, `value.bool`, `value.string`.
    *   Example: A slider connected to an action `set_brightness` would use an expression like `set: { brightness: value.float }`.
*   **Multiple `when` clauses:** If you list multiple `when` blocks under one action, they behave like an `OR`. The action will execute the `then` block of the *first* `when` whose condition is met. This is useful for state-machine-like behavior.

### 3.4. `updates` Block Deep Dive

The `updates` block is a list of modifications that run on every tick. It's the engine for continuous simulation.

```yaml
updates:
  # Unconditional update: always increments time_elapsed if printing is true
  - when:
      condition: printing
      then:
        - inc: { time_elapsed: 1 }

  # Conditional update within a modifier
  - current_temp: { inc: 0.1, when: [<, current_temp, target_temp] }
```

---

## 4. THE EXPRESSION LANGUAGE

This is a LISP-style S-expression language. An expression is either a literal/reference or a list `[<function>, <arg1>, <arg2>, ...]`.

| Category | Syntax | Description |
| :--- | :--- | :--- |
| **Literals** | `123.4`, `true`, `false`, `"some text"` | Raw values. |
| **References** | `state_name`, `!boolean_state`, `time`, `value.float` | Accesses the value of a state variable, the special time variable, or the action payload. |
| **Arithmetic** | `[add, A, B, ...]`, `[sub, A, B]`, `[mul, A, B, ...]`, `[div, A, B]` | Basic math. `add` and `mul` can take multiple arguments. |
| **Trigonometry**| `[sin, A]`, `[cos, A]` | Input `A` must be in **radians**. |
| **Clamping** | `[clamp, val, min, max]` | Constrains `val` to be between `min` and `max`. |
| **Comparison**| `[==, A, B]`, `[!=, A, B]`, `[>, A, B]`, `[<, A, B]`, `[>=, A, B]`, `[<=, A, B]` | Compares numbers, booleans, or strings. Returns `true` or `false`. |
| **Logic** | `[and, C1, C2, ...]`, `[or, C1, C2, ...]`, `[not, C]` | Logical operations. |

### The `case` Expression

The `case` expression is the equivalent of an `if/else if/else` chain. It is powerful for creating state machines.

**Syntax:** `{ case: [ [condition1, value1], [condition2, value2], [true, else_value] ] }`

It evaluates each `[condition, value]` pair in order. It returns the `value` of the **first** pair whose `condition` is `true`. The final `[true, else_value]` acts as the `else` block.

```yaml
# Example: Convert a numeric step into a human-readable string
- status_string:
    derived_expr:
      case:
        - [[==, setup_step, 0], "Welcome"]
        - [[==, setup_step, 1], "Select Strength"]
        - [[==, setup_step, 2], "Select Size"]
        - [true, "Ready"]
```

---

## 5. SIMULATOR INTERNALS & CRITICAL GOTCHAS

To generate correct simulations, you must understand the exact order of operations within a single simulator tick.

#### **Execution Order Per Tick**

1.  **Execute Scheduled Actions:** Any actions from the `schedule` block for the current tick are run first.
2.  **Execute `updates` Block:** All modifications in the `updates` list are evaluated and executed. **Crucially, all expressions in this step use the `state` values from the *beginning* of the tick.** For example, if `time` is 5.0 at the start of the tick, all `updates` calculations will use `time = 5.0`.
3.  **Increment `time`:** The special `time` variable is incremented by the time delta (a small float, typically ~0.033).
4.  **Recalculate Derived States:** All `derived_expr` states are re-evaluated based on the new, potentially modified state values. This happens *after* all actions and updates for the tick are complete.

**This order is paramount.** A common mistake is assuming `time` is updated before the `updates` block runs. It is not.

#### **Source Code Insights for Precision**

To resolve any ambiguity, here are key snippets from the simulator's C source code.

*   **Tick Execution Order (`ui_sim_tick`):**

    ```c
    void ui_sim_tick(float dt) {
        if (!g_sim.is_active) return;
        g_sim.current_tick++;
        // 1. Execute scheduled actions for this tick.
        for (...) { sim_action_handler(...); }
        // 2. Run the updates block.
        execute_modifications_list(g_sim.updates_head, ...);
        // 3. Increment time at the end of the tick logic.
        SimStateVariable* time_state = find_state("time");
        if (time_state) { ... time_state->value += dt; ... }
        // 4. Notify UI (which includes re-evaluating derived expressions).
        notify_changed_states();
    }
    ```

*   **Floating Point Comparison (`values_are_equal`):** The `==` operator for floats uses an epsilon, so minor floating-point inaccuracies are tolerated.

    ```c
    case BINDING_TYPE_FLOAT:
        return fabsf(v1.as.f_val - v2.as.f_val) < 1e-6f;
    ```

---

## 6. COMPREHENSIVE EXAMPLES

Study these examples to understand how to combine the concepts to solve real-world problems.

### Example 1: Thermostat

*   **Goal:** Adjust a target temperature. A simulated current temperature slowly "moves" towards the target. An indicator for the heater turns on/off.
*   **Concepts:** `inc`/`dec` in actions, conditional `inc`/`dec` in updates, `case` expression for derived-like logic.

```yaml
- type: data-binding
  state:
    - current_temp: 22.0
    - target_temp: 22.0
    - heater_on: false
  actions:
    - temp_up: { inc: { target_temp: 0.5 } }
    - temp_down: { dec: { target_temp: 0.5 } }
  updates:
    - current_temp: { inc: 0.1, when: [<, current_temp, target_temp] }
    - current_temp: { dec: 0.1, when: [>, current_temp, target_temp] }
    - heater_on:
        set:
          case:
            - [[>, [sub, target_temp, current_temp], 0.2], true]
            - [true, false]
```

### Example 2: 3D Printer Progress

*   **Goal:** Simulate a print job with progress percentage and time remaining.
*   **Concepts:** `updates` block gated by a boolean (`printing`), using `time_elapsed` to drive calculations, `clamp` to keep percentage between 0-100.

```yaml
- type: data-binding
  state:
    - printing: false
    - progress_pct: 0.0
    - time_elapsed: 0.0
    - time_total: 3600.0
    - time_remaining: 3600.0
  actions:
    - start_print: { set: { printing: true, progress_pct: 0.0, time_elapsed: 0.0 } }
    - cancel_print: { set: { printing: false } }
  updates:
    - when:
        condition: printing
        then:
          - inc: { time_elapsed: 1 } # Assume 1 tick = 1 second
          - set:
              progress_pct: [clamp, [mul, [div, time_elapsed, time_total], 100], 0, 100]
          - set: { time_remaining: [sub, time_total, time_elapsed] }
          - when:
              condition: [>=, progress_pct, 100]
              then:
                - set: { printing: false }
```

### Example 3: Audio Waveform Selector

*   **Goal:** A button that cycles through different modes. A slider that sets a value.
*   **Concepts:** `cycle` modifier for strings, `value.float` for action payloads.

```yaml
- type: data-binding
  state:
    - waveform: [string, "SINE"]
    - frequency: 440.0
  actions:
    - cycle_waveform:
        cycle: { waveform: ["SINE", "SQUARE", "SAW", "TRIANGLE"] }
    - set_frequency:
        set: { frequency: value.float }
```

### Example 4: Advanced CNC Control Panel

*   **Goal:** A complex simulation with multiple coordinate systems, jogging, probing, and derived readouts.
*   **Concepts:** `derived_expr` for complex readouts, `case` for state-dependent logic, `when` clauses to target specific WCS, using state variables to store geometry for probing actions.

```yaml
- type: data-binding
  state:
    # -- Machine State --
    - time: 0.0
    - program_running: false
    - spindle_on: false
    # -- Machine Absolute Position (G53) --
    - pos_x: 10.0
    - pos_y: 15.0
    - pos_z: 50.0
    # -- Work Coordinate Systems (WCS) --
    - wcs_name: [string, "G54"] # Active WCS
    - wcs54_x_zero: 250.5
    - wcs54_y_zero: 175.2
    - wcs54_z_zero: 100.0
    - wcs55_x_zero: 450.0
    - wcs55_y_zero: 175.2
    - wcs55_z_zero: 120.5
    # -- Overrides --
    - jog_speed: 10.0 # mm per click
    - spindle_rpm: 0.0
    # -- DERIVED STATE --
    - program_status:
        derived_expr:
          case:
            - [program_running, "RUNNING"]
            - [spindle_on, "SPINDLE ON"]
            - [true, "IDLE"]
    - wcs_pos_x:
        derived_expr:
          case:
            - [[==, wcs_name, "G54"], [sub, pos_x, wcs54_x_zero]]
            - [[==, wcs_name, "G55"], [sub, pos_x, wcs55_x_zero]]
            - [true, 0.0]
    # ... wcs_pos_y and wcs_pos_z are similar ...
  actions:
    - program_run: { set: { program_running: true, spindle_on: true } }
    - program_stop: { set: { program_running: false, spindle_on: false } }
    - cycle_wcs: { cycle: { wcs_name: ["G54", "G55"] } }
    - zero_wcs_x:
        - when: { condition: [==, wcs_name, "G54"], then: { set: { wcs54_x_zero: pos_x } } }
        - when: { condition: [==, wcs_name, "G55"], then: { set: { wcs55_x_zero: pos_x } } }
    # ... zero_wcs_y and zero_wcs_z are similar ...
    - jog_x_plus:  { inc: { pos_x: jog_speed } }
    - jog_x_minus: { dec: { pos_x: jog_speed } }
  updates:
    - set:
        spindle_rpm:
          case:
            - [spindle_on, [add, 8000, [mul, 200, [sin, [mul, time, 10]]]]]
            - [true, 0]
    - when:
        condition: program_running
        then:
          - set: { pos_x: [add, 200, [mul, 50, [cos, [mul, time, 0.5]]]] }
          - set: { pos_y: [add, 150, [mul, 50, [sin, [mul, time, 0.5]]]] }
          - range: { pos_z: [10, 20, 0.2] }
```

---

## 7. YOUR TASK: LLM's Thinking Process

When a user provides a problem, you will follow these steps to construct the `data-binding` block:

1.  **Identify all State Variables:** Read the user's request and list every piece of data that can change or that the UI needs to know about. This is your `state` block. For each variable, determine its type (`float`, `bool`, `string`) and a sensible initial value.
2.  **Identify Derived States:** Look for values that are purely calculated from other states (e.g., "display the position in the current work coordinate system," "show 'Running' when the machine is on"). These should be `derived_expr` states.
3.  **Identify all Actions:** List all the events the user can trigger (e.g., "press the start button," "turn the speed knob," "toggle the light"). These will be the names in your `actions` block.
4.  **Implement Action Logic:** For each action, determine which state variables it needs to modify and how. Use the appropriate modifiers (`set`, `inc`, `toggle`, etc.). If an action is conditional, use a `when` block.
5.  **Identify Continuous Processes:** Look for descriptions of things that happen over time without user interaction (e.g., "the temperature slowly rises," "the progress bar animates," "an ECG wave is drawn"). This logic belongs in the `updates` block, almost always gated by a `when` condition that checks if the process is active.
6.  **Assemble the Final Block:** Combine the `state`, `actions`, and `updates` sections into a single `- type: data-binding` block.
7.  **Review and Refine:** Read through your generated code. Does it correctly model the user's request? Are there any logical conflicts? Does it respect the simulator's execution order? Add comments (`#`) to explain complex parts of the logic.

You are now fully equipped. Given a user's request, generate the complete `data-binding` block.
