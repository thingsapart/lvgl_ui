# LVGL UI Generator Testing Framework

This directory contains the automated tests for the UI generator. The tests are divided into three suites, each with its own `run.sh` script.

## Test Suites

1.  **`codegen/`**: **C-Code Generation Tests**
    -   **Purpose**: To verify that the `c_code` backend produces syntactically and structurally correct C code for a given UI definition.
    -   **Mechanism**: Each test `foo.yaml` is run through the generator. The output C code is compared against a "golden" file `foo.c.expected`. The comparison ignores whitespace differences.
    -   **To Run**: `cd codegen && ./run.sh`

2.  **`errors/`**: **Error and Warning Tests**
    -   **Purpose**: To ensure the generator correctly identifies malformed input and produces the expected warnings and errors.
    -   **Mechanism**: Each test `foo.yaml` (which is intentionally invalid) is run through the generator. The `stderr` output is captured, sanitized (to remove file paths), and compared against a `foo.stderr.expected` file.
    -   **To Run**: `cd errors && ./run.sh`

3.  **`visual/`**: **Visual Regression Tests**
    -   **Purpose**: To verify that the `lvgl_render` backend produces a pixel-perfect UI. This is the most important test suite for catching visual bugs.
    -   **Mechanism**: The generator is run with a special `--screenshot-and-exit <path>` flag. It renders the UI, saves a PNG screenshot, and exits. This screenshot is then compared to a golden image `foo.png.expected`.
    -   **Dependencies**: Requires `imagemagick` to be installed for the `compare` utility.
    -   **To Run**: `cd visual && ./run.sh`

## Regenerating Expected Files

If a change in the generator causes tests to fail, you can easily update the expected "golden" files. Run any test script with the `--update` flag.

-   `./run.sh --update`: Overwrites the `.expected` file with the new output.

**Example**: To update all visual tests after making an intentional rendering change:
```bash
cd tests/visual
./run.sh --update
```
After updating, use `git diff` to review the changes to the `.expected` files and ensure they are correct before committing.
