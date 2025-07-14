`lvgl_ui_generator` is a code generator that translates a declarative UI definition (`ui.json`) into procedural C code. It operates in three main stages:

1.  **API Specification Parsing:** The `api_spec.json` file, which describes the LVGL API (widgets, functions, enums, properties), is parsed into an in-memory, queryable data structure called `ApiSpec`. This spec acts as a "database" for the generator, providing essential information like function signatures, C types, and relationships between properties and their setter functions.
2.  **UI-to-IR Generation:** The `ui.json` file is parsed. The generator (`generator.c`) recursively walks through the JSON tree. For each JSON object representing a widget or style, it consults the `ApiSpec` to understand how to create and configure it. It translates the declarative JSON into an **Intermediate Representation (IR)**. This IR is a tree structure that represents the sequence of C function calls needed to build the UI.
3.  **Backend Code Generation:** The generated IR tree is passed to a backend processor. The primary backend, `c_code_printer.c`, traverses the IR tree and prints a C source file (`create_ui` function) that, when compiled and run, will construct the UI described in the original `ui.json`.

---

### API-SPEC Parsing

The `api_spec.json` file is a pre-processed description of the LVGL API, likely created by a tool like the provided `generate_api_spec.py`. The C application processes this file as follows:

-   **Entry Point:** In `main.c`, the JSON file content is read and parsed into a `cJSON` object. This object is passed to `api_spec_parse()` in `api_spec.c`.
-   **Parsing Logic (`api_spec_parse`):** This function allocates an `ApiSpec` struct and populates it by iterating through the top-level keys of the JSON:
    -   `"widgets"` & `"objects"`: Each entry is parsed into a `WidgetDefinition` struct, which contains its name, what it inherits from, its creation function (e.g., `lv_btn_create`), its C type (e.g. `lv_style_t`), and a list of its specific properties and methods. These are stored in a linked list (`widgets_list_head`).
    -   `"functions"`: Global functions are parsed into `FunctionDefinition` structs, storing their name, return type, and argument list.
    -   `"enums"` & `"constants"`: These sections are kept as raw `cJSON` pointers within the `ApiSpec` struct for direct, efficient lookup later.
-   **Purpose:** The resulting `ApiSpec*` is a structured, in-memory representation of the API. The generator heavily relies on it to find information via functions like `api_spec_find_widget`, `api_spec_find_property`, and `api_spec_get_function_return_type`.

---

### UI-SPEC Parsing and IR Generation (Detailed)

This is the core of the application, transforming the `ui.json` into the IR. The process is orchestrated by `generator.c`.

#### Conceptual Overview

The generator's goal is to convert the *declarative* nature of JSON (defining "what is") into a *procedural* sequence of operations (defining "how to build it").

-   A JSON object like `{ "type": "button", "id": "@my_btn", "width": 100 }`
-   ...is translated into IR that represents the C code:
    1.  `lv_obj_t* my_btn_0 = lv_button_create(parent);` (Constructor in `IRObject->constructor_expr`)
    2.  `lv_obj_set_width(my_btn_0, 100);` (Setup Call in `IRObject->operations` list)

#### Detailed Functional Flow

1.  **Entry Point (`generate_ir_from_ui_spec`):**
    -   This function is called from `main.c` after the API spec is parsed.
    -   It initializes a `GenContext` which holds the `ApiSpec`, a `Registry` for tracking generated variable names, and a counter for unique name generation.
    -   It iterates through the top-level array of the `ui_spec.json`. For each JSON object in the array, it calls the main recursive parser, `parse_object`.

2.  **Object Parsing (`parse_object`):**
    -   This function is the heart of the generator and handles a single JSON object.
    -   **Identification:** It reads the `type` and `id` keys. The `type` determines the kind of object (e.g., "button", "style"), defaulting to "obj". The `id` (e.g., `@my_button`) is used for registering the object so other parts of the UI can reference it.
    -   **IR Node Creation:** It creates a new `IRObject` node using `ir_new_object`. It generates a unique C variable name (e.g., `my_button_0`) using `generate_unique_var_name`. It also determines the object's C type (e.g., `lv_obj_t*` or `lv_style_t*`) by querying the `ApiSpec`.
    -   **Constructor Generation:**
        -   If the JSON has an `"init": { ... }` block, it's treated as an explicit constructor function call. `unmarshal_value` is called to generate an `IRExprFunctionCall`.
        -   Otherwise, it looks up the widget type in the `ApiSpec` to find its `create` function (e.g., `lv_label_create`) or `init` function (e.g., `lv_style_init`). It then creates an `IRExprFunctionCall` for this function. The first argument is always the parent's C variable name.
        -   This constructor expression is stored in the `IRObject->constructor_expr` field.
    -   **Property/Method/Child Processing:** It iterates over all other keys in the JSON object (e.g., `"text"`, `"width"`, `"add_flag"`, `"children"`).
        -   **Properties/Methods:** For each key, it calls `api_spec_find_property()` to find the corresponding LVGL setter function (e.g., `width` -> `lv_obj_set_width`). It then converts the JSON property's value into an `IRExpr` and creates an `IRExprFunctionCall` node for the setter. This function call is added to the `IRObject->operations` linked list.
        -   **Children:** If a `"children"` key exists, it recursively calls `parse_object` for each child. The resulting child `IRObject` node is also added to the current object's `operations` list, ensuring children are created after the parent is configured.
        -   **Data Binding:** Keys like `"observes"` and `"action"` are translated into `IRObserver` and `IRAction` nodes, which are also added to the `operations` list.

3.  **Value Unmarshaling (`unmarshal_value`):**
    -   This utility function converts a `cJSON` value into a specific `IRExpr` node. It's crucial for handling different value types.
    -   `cJSON_IsString`:
        -   `"@name"` -> `ir_new_expr_registry_ref()`: Creates a reference to another object.
        -   `"!string"` -> `ir_new_expr_static_string()`: Creates a string that will be persisted.
        -   `"#RRGGBB"` -> `ir_new_expr_func_call("lv_color_hex", ...)`: Creates a call to generate an `lv_color_t`.
        -   `"SYMBOL"` -> `ir_new_expr_enum()`: Creates an enum expression if the string is found in the `ApiSpec`.
        -   `"other string"` -> `ir_new_expr_literal_string()`: A regular string literal.
    -   `cJSON_IsNumber`: -> `ir_new_expr_literal()`: A numeric literal.
    -   `cJSON_IsBool`: -> `ir_new_expr_literal("true" or "false")`.
    -   `cJSON_IsObject`: If the object is `{"func_name": [...]}` it's treated as a function call and becomes an `IRExprFunctionCall`.

---

### The Intermediate Representation (IR)

The IR is defined in `ir.h` and `ir.c`. It's a tree of nodes that represents the UI structure and the operations to create it. It decouples the initial JSON parsing from the final code generation.

-   **`IRNode`**: The base struct with a `type` enum. All other IR nodes start with this.

-   **`IRObject`**: The primary structural node.
    -   `c_name`: The unique, generated C variable name (e.g., `label_0`).
    -   `json_type`: The original type from JSON (e.g., `"label"`).
    -   `c_type`: The actual C type (e.g., `lv_obj_t*`).
    -   `registered_id`: The user-provided ID from JSON (e.g., `@my_label`).
    -   `constructor_expr`: An `IRExpr*` that generates the code to create the object (e.g., a call to `lv_label_create(parent)`).
    -   **`operations`**: This is a crucial linked list of `IROperationNode`s. It contains an ordered sequence of all actions to be performed on or within the context of this object. This includes:
        -   Setup calls (e.g., `lv_label_set_text(...)`).
        -   Child object creation (`IRObject` nodes).
        -   Data binding setup (`IRObserver`, `IRAction` nodes).
        -   Generator warnings (`IRWarning` nodes).

-   **`IRExpr` Family**: These nodes represent values and expressions.
    -   **`IRExprLiteral`**: Represents a literal value like `100`, `"Hello"`, or `true`.
    -   **`IRExprEnum`**: Represents an LVGL enum like `LV_ALIGN_CENTER`.
    -   **`IRExprFunctionCall`**: Represents a function call, like `lv_obj_set_width(...)` or `lv_color_hex(...)`. This is the fundamental building block of the procedural output.
    -   **`IRExprRegistryRef`**: Represents a reference to another object by its ID, like `@my_label`. The C-Code backend will resolve this to a C variable name like `my_label_0`.
    -   The other expression types (`IR_EXPR_STATIC_STRING`, `IR_EXPR_CONTEXT_VAR`, `IR_EXPR_ARRAY`) handle more specific cases.

---

### Output Backends (C-Code Backend Focus)

A backend is a module that consumes the `IRRoot` and produces an output. The `c_code_print_backend` function in `c_code_printer.c` is responsible for generating the final C code.

It works in two passes:

**Pass 1: Building the ID Map (`build_id_map_recursive`)**

-   The IR contains symbolic references (`@my_label`) but the C code needs concrete variable names (`label_0`).
-   This function recursively traverses the *entire* IR tree *before* printing any code.
-   It populates an `IdMapNode` linked list. This map creates a two-way association between a symbolic ID (`@my_label`) and its generated C variable name (`label_0`) and C type (`lv_obj_t*`). This map is essential for resolving references during code generation.

**Pass 2: Generating C Code (`print_object_list`)**

-   This is a recursive function that traverses the `IRObject` list and prints C code to `stdout`.
-   For each `IRObject`:
    1.  It prints a `do { ... } while(0);` block to create a new C scope for the object and its children.
    2.  It prints the variable declaration based on the `c_type` and `c_name` from the `IRObject` node (e.g., `lv_obj_t* label_0;`).
    3.  It calls `print_expr` on the `IRObject->constructor_expr`. This generates the assignment line, e.g., `label_0 = lv_label_create(parent);`.
    4.  It iterates through the `IRObject->operations` list. For each operation, it calls `print_node`, which handles printing child objects, setup calls, warnings, etc., in the correct order.
    5.  The recursion for children happens within the `print_node` function when it encounters an `IR_NODE_OBJECT` in the operations list.

**Expression Printing (`print_expr`)**

-   This function is the inverse of `unmarshal_value`. It takes an `IRExpr*` and prints its C code representation.
-   `IR_EXPR_LITERAL` -> prints the value (e.g., `100` or `"some_text"`).
-   `IR_EXPR_ENUM` -> prints the symbol (e.g., `LV_ALIGN_CENTER`).
-   `IR_EXPR_FUNCTION_CALL` -> prints the function name, an opening parenthesis, recursively calls `print_expr` for each argument, and prints a closing parenthesis.
-   `IR_EXPR_REGISTRY_REF`: This is where the pre-built `IdMap` is used. It looks up the reference name (e.g., `@my_label` or `parent`) in the map and prints the corresponding C variable name (e.g., `label_0` or `parent`). It also intelligently adds a `&` prefix if the mapped C type is not a pointer (e.g., for `lv_style_t`).
