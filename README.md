# LVGL UI Generator

*From Declarative Blueprints to Procedural C Code*

This project is a transpiler that converts high-level, human-readable UI definitions in YAML into efficient, procedural C code for the [LVGL](https://lvgl.io/) graphics library. It provides a structured pipeline for defining user interfaces, treating them as a language to be parsed, analyzed, and compiled.

- [Advantages](#advantages)
- [The Core Philosophy](#the-core-philosophy)
- [The Generation Pipeline](#the-generation-pipeline)
- [Key Features](#key-features)
- [Getting Started](#getting-started)

## Advantages

*   **Declarative by Design**: Describe *what* your UI should be in YAML, not *how* to construct it step-by-step. This can simplify layout, styling, and widget hierarchy management.
*   **Rapid Prototyping**: Iterate on UI designs, tweak properties, and refactor layouts by editing a simple text file, often without recompiling your core application logic.
*   **Decoupling of Concerns**: Encourages a separation between the UI definition (the "View") and the application logic (the "Controller"), which can lead to more maintainable codebases.
*   **Powerful Data Binding**: Implements a data binding system with **Actions** (UI to App) and **Observers** (App to UI), a feature not native to LVGL.
*   **Component Reusability**: Define reusable UI components with the `use-view` directive, enabling the creation of consistent and complex interfaces from simple building blocks.
*   **Code Consistency**: The generator produces predictable, standardized C code, which can help eliminate manual inconsistencies and boilerplate errors.

## The Core Philosophy

LVGL is a powerful, imperative C library. While this provides fine-grained control, it means that UI construction is inherently procedural: `create widget -> set property -> create child -> set property...`. This process can become verbose and difficult to visualize or maintain for complex screens.

This generator re-frames the problem. It treats a UI specification as a formal document to be transpiled. The system ingests a declarative format (YAML) and outputs a procedural one (C), much like a high-level programming language is compiled to machine code. This is achieved through a multi-stage pipeline that enables correctness and abstractions.

## The Generation Pipeline

The process from a `.yaml` file to C code follows a structured, three-stage pipeline:

1.  **API Specification (`api_spec.json`)**
    The process begins by parsing a formal schema of the LVGL API. This JSON file acts as the "grammar" for the generator, defining all available widgets, their properties, setter functions, C types, and enumerations. It is the single source of truth that makes the entire system type-aware.

2.  **Intermediate Representation (IR) Generation**
    The input `ui.yaml` is parsed into an in-memory **Intermediate Representation (IR)**. This IR is a structured tree of nodes, akin to an Abstract Syntax Tree (AST), that represents the sequence of C function calls required to build the UI. This decoupling layer allows for analysis, validation, and the ability to target different backends.

3.  **Backend Code Generation**
    The IR tree is fed to a backend processor. The primary backend is a C-code printer that traverses the IR and emits a well-formatted `.c` file containing a `create_ui(lv_obj_t *parent)` function. Other backends include a live SDL renderer for visual testing and various debug printers.

## Key Features

### YAML-Driven UI
The primary input format is YAML. This was chosen for its readability, support for comments, and, critically, the ability of our custom parser to handle **duplicate keys**. This allows for a natural syntax for applying multiple operations of the same type, such as adding several styles to a widget.

```yaml
- type: button
  # Applying multiple styles is clean and intuitive
  add_style: ['@style_btn', LV_PART_MAIN]
  add_style: ['@style_btn_danger', LV_PART_MAIN]
```

### Data Binding System
A relationship between your UI and application state is made possible through a built-in data binding system.

*   **Actions (UI → App)**: Widgets can trigger named actions. A single, centralized C handler receives these events, separating UI events from application logic.
*   **Observers (App → UI)**: Widgets can observe application state variables. When your application notifies the system of a state change, the UI automatically updates widget properties like text, visibility, or styling.

See the [Data Binding Documentation](./data-binding.md) for a complete guide.

### Component System
Promote reusability with the `use-view` directive. Define a common widget or layout as a `component` and instantiate it elsewhere, optionally overriding its default properties.

```yaml
# Define a reusable component
- type: component
  id: "@fancy_button"
  content:
    type: button
    width: 150
    add_style: ["@style_btn", 0]
    children:
      - type: label
        text: "Default Text"

# Instantiate it elsewhere
- type: use-view
  id: "@fancy_button"
  text: "Click Me!" # Override the label's text
  action: { do_something: trigger }
```

## Getting Started

To generate a C file from your UI definition, run the generator from the command line:

```bash
# This command generates a C file that will create the UI
./lvgl_ui_generator api_spec.json my_ui.yaml --codegen c_code > create_ui.c
```

The resulting `create_ui.c` file can be compiled directly into your LVGL project. Simply call the `create_ui(parent_object)` function to build the interface on any given container.
