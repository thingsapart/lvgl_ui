### Summary of the JSON UI-SPEC Format

This is a declarative UI format for the LVGL graphics library, expressed in JSON or YAML. It allows developers to define complex user interfaces, including reusable components and styling, which are then programmatically generated into LVGL C objects.

The core of the format revolves around a few key concepts:

1.  **Widget Tree:** The UI is defined as a hierarchical tree of widget objects, similar to how LVGL itself works. Each object can have properties and children.
2.  **C-Code Interoperability (Registry):** A "registry" acts as a bridge between the declarative UI and the host C application. The C code can register pointers (like fonts, images, or C variables), which can then be referenced by name within the UI definition (e.g., `text_font: @my_font`). Conversely, UI elements can be given a `named` attribute to register them for access from the C code.
3.  **Components & Reusability:** UI sections can be defined as reusable `component` blocks. These components can then be instantiated multiple times using a `use-view` block, promoting a modular design.
4.  **Parameterization (Context):** Components are parameterized using a `context`. When a component is used (`use-view`), values are passed into its context. Inside the component definition, these values are accessed with a `$` prefix (e.g., `text: $title`).
5.  **Styling:** The format supports both the definition of global, reusable `lv_style_t` objects and the application of local style properties directly on a widget.
6.  **Shorthands and Abstractions:** The format simplifies development by providing several shorthands. Property names are automatically resolved to corresponding LVGL setter functions (e.g., `flex_flow` becomes `lv_obj_set_flex_flow`). Special prefixes are used for values like colors (`#rrggbb`), percentages (`50%`), registered pointers (`@name`), and context variables (`$name`).

In essence, the format provides a high-level, human-readable abstraction over the LVGL C API, enabling rapid UI development, theming, and clear separation between UI layout and application logic.

---

### YAML/JSON Include Directives

The generator supports including external YAML or JSON files directly from a UI-SPEC. Includes are processed during parsing by `generator.c` and support a small set of options.

- Basic form (string):

```
- include: other_parts.yaml
```

- Object form with options:

```
 - include:
     file: components/buttons.yaml
     context:
       title: "OK"
       font: my_font
     as: myprefix
     merge: true
```

Key behaviors implemented by the generator:

- `file` / string: The include target path (relative paths are resolved against the including file's directory).
- `context`: An object merged with the current UI context for the scope of the included content. Values from the include's `context` override values from the outer context when keys conflict.
- `as`: If provided, all `id` string values inside the included content are prefixed with `as` + `_` (for example `btn1` becomes `myprefix_btn1`). IDs that already start with `@` are considered global and are NOT prefixed.
- `merge`: Boolean flag (default true). The included file must contain a top-level YAML/JSON array of UI elements; the elements are processed into the including document's object list. (Note: current generator implementation treats `merge=false` the same as `merge=true`.)
- Included files are parsed via the same YAML-to-cJSON path as top-level specs; non-array included content will produce a warning.

Context variable and key interpolation interplay:

- The generator supports `$`-prefixed keys in object maps, which are replaced at parse time using the active context. For example, an object key named `$label` will be replaced by the string value held in the context under `label`.
- A key may use the inline-doc form `$name/doc-info`; the portion before the slash (`name`) is looked up in the context and its string value is used as the final key name.

Example - include with context and id-prefixing:

```
- include:
    file: controls/toolbar.yaml
    context:
      title: "Main"
    as: toolbar

# In controls/toolbar.yaml (each file must be a top-level array)
- type: label
  id: title
  text: $title

# After include the label id becomes `toolbar_title` and $title -> "Main" for the included scope.
```

---

### Formalized Pseudo-Code Description

The following pseudo-code describes the structure of the UI definition.

#### Root Element

A UI definition is a list of top-level elements.

```
UI_Definition ::= List<TopLevelElement>
```

#### Top-Level Elements

These are the primary building blocks that can appear at the root of the definition.

```
TopLevelElement ::= Widget | ComponentDefinition | StyleDefinition
```

#### Element Definitions

**1. Widget**
The fundamental UI element. It represents an LVGL object (`lv_obj_t`) and its derivatives (`lv_label_t`, `lv_button_t`, etc.). If `type` is omitted, it defaults to a base object (`lv_obj_t`), useful as a container.

```
Widget {
  type: string                 // Optional. The LVGL widget type (e.g., "label", "button", "bar").
  named: string                // Optional. Registers this widget instance with the given name.
  children: List<Widget>       // Optional. A list of child widgets.
  with: List<WithBlock>        // Optional. Apply properties to a sub-object or expression result, with is a properyy like and can bre repeated.
  deferred: boolean | string   // Optional. Extract this widget's children into a separate static C function (see below).
  ...properties: PropertyMap   // Any number of widget-specific properties, properties can be repeated.
}
```
*   A special widget type `use-view` is used to instantiate a component (see below).

**2. ComponentDefinition**
Defines a reusable UI template. It is not rendered directly but is referenced by a `use-view` widget.

```
ComponentDefinition {
  type: "component"            // Required.
  id: "@component_name"        // Required. The unique identifier for this component.
  content: Widget              // Required. The root widget definition for this component.
}
```

**3. StyleDefinition**
Defines a reusable `lv_style_t` object that is automatically registered.

```
StyleDefinition {
  type: "style"                // Required.
  id: "@style_name"            // Required. The unique identifier to register this style.
  ...properties: StylePropertyMap // Any number of style properties (e.g., `radius`, `bg_color`).
}
```

**4. UseView (Component Instantiation)**
A special type of `Widget` that instantiates a defined `Component`.

```
UseView {
  type: "use-view"             // Required.
  id: "@component_name"        // Required. References a defined component.
  context: Map<string, Value>  // Optional. Provides key-value pairs for parameterization.
  ...properties: PropertyMap   // Optional. Overrides for the component's root widget properties.
}
```

---

#### Deferred UI Functions

The `deferred` property enables **lazy page loading**: the children of a widget are extracted from `create_ui` into a separate `static void` C function that you call from your application only when that content is actually needed (e.g., when the user navigates to a tab or page). This is especially useful on memory-constrained targets like the ESP32-S3 where constructing every page at startup would exhaust the heap.

**Syntax**

| Value | Effect |
|-------|--------|
| `deferred: true` | Auto-generate a function named `create_ui_<c_var>` where `<c_var>` is the widget's generated C variable name (e.g. `create_ui_tab_home_0`). |
| `deferred: "my_fn"` | Use the explicitly given name `my_fn` as the function name. |

The widget itself (constructor + own properties) is still created inside `create_ui`; only its *children* are deferred.

**Example YAML**

```yaml
- type: tabview
  children:
    - named: tab_home
      deferred: true          # auto-names create_ui_tab_home_0
      children:
        - type: label
          text: "Home Content"
          align: LV_ALIGN_CENTER
    - named: tab_settings
      deferred: "build_settings_page"   # explicit function name
      children:
        - type: label
          text: "Settings"
```

**Generated C code** (from `c_code` backend)

```c
// --- Deferred UI Functions ---

static void create_ui_tab_home_0(lv_obj_t* parent) {
    do {
        lv_obj_t* label_0 = lv_label_create(parent);
        lv_label_set_text(label_0, "Home Content");
        lv_obj_set_style_align(label_0, LV_ALIGN_CENTER, 0);
    } while(0);
}

static void build_settings_page(lv_obj_t* parent) {
    do {
        lv_obj_t* label_1 = lv_label_create(parent);
        lv_label_set_text(label_1, "Settings");
    } while(0);
}

void create_ui(lv_obj_t* parent) {
    do {
        lv_obj_t* tabview_0 = lv_tabview_create(parent);
        do {
            lv_obj_t* tab_home_0 = lv_tabview_add_tab(tabview_0, "Home");
            create_ui_tab_home_0(tab_home_0);     // ← deferred call
        } while(0);
        do {
            lv_obj_t* tab_settings_0 = lv_tabview_add_tab(tabview_0, "Settings");
            build_settings_page(tab_settings_0);  // ← deferred call
        } while(0);
    } while(0);
}
```

**Backend behaviour**

| Backend | Behaviour |
|---------|----------|
| `c_code` | Emits deferred functions before `create_ui`; replaces inlined children with a call. |
| `ir_debug_print` | Annotates the object line with `deferred="fn_name"`. |
| `lvgl_renderer` (live SDL preview) | Ignores `deferred`; always renders children inline so the preview works without any extra calls. |

---

#### Attribute Groups & Value Types

**PropertyMap**
A map of key-value pairs where keys are shorthand LVGL property names (e.g., `text`, `width`, `align`, `add_style`) and values can be of type `Value`.

```
PropertyMap ::= Map<string, Value>
```
*   **Property Name Resolution:** A key like `prop_name` on a widget of `type_name` is resolved to a C function by trying `lv_type_name_set_prop_name()`, then `lv_obj_set_prop_name()`.

**WithBlock**
Applies a set of properties to a sub-object, such as the list part of a dropdown.

```
WithBlock {
  obj: Value | FunctionCall    // Required. An expression that yields an object to work with.
  do: PropertyMap              // Required. Properties to apply to the object from 'obj'.
}
```

**FunctionCall**
Represents a direct call to an LVGL function.

```
FunctionCall {
  call: string                 // Required. The name of the LVGL function to call.
  args: List<Value>            // Optional. A list of arguments for the function.
}
```

**Value**
Represents the value of a property. It can be a standard JSON/YAML type or one of the special formats.

```
Value ::=
  | string                      // A standard string.
  | number                      // A number.
  | boolean                     // true or false.
  | Array<Value>                // A list of values (e.g., for size `[100, 50]`).
  | FunctionCall                // The result of an LVGL function call.
  | SpecialValueString          // A string with a special prefix indicating its type.
```

**SpecialValueString Formats**

| Prefix | Format         | Description                                        | Example                   |
|--------|----------------|----------------------------------------------------|---------------------------|
| `@`    | `@name`        | **Registry Reference:** A pointer to an object defined *within* the UI spec. | `text_font: @my_font`     |
| `@$`   | `@$name`       | **External C Identifier:** A direct reference to an external C variable (e.g., an image struct). | `src: @$my_image_data`    |
| `$`    | `$name`        | **Context Variable:** A value from the current context. | `text: $title`            |
| `!`    | `!string`      | **Static String:** A heap-allocated, persistent string. | `options: !'A\nB\nC'`      |
| `#`    | `#RRGGBB`      | **Color:** A hex color, resolved to `lv_color_hex()`.| `bg_color: #ff0000`       |
| `%`    | `N%` (suffix)  | **Percentage:** A percentage, resolved to `lv_pct(N)`. | `width: 50%`              |
| `?|`   | `?|static|dyn` | **Conditional:** `static` for C-code, `dyn` for live preview. | `src: '?|@$img_name|S:~/img.png'` |

**Value Unescaping**
To use the special characters (`$`, `!`, `@`, `%`, `#`) literally in a string, they must be escaped by doubling them.

| Escaped     | Unescaped Result |
|-------------|------------------|
| `$$name$$`  | `$name$`         |
| `!!name!!`  | `!name!`         |
| `@@name@@`  | `@name@`         |
| `100%%`     | `100%`           |
| `##name##`  | `#name#`         |
