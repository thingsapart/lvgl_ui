# Important information

lvgl_ui generates a UI based on two JSON files - one that specifies the full LVGL API (ie all functions, objects, widgets and enum values) called the API-SPEC, usually in ./api_spec.json, and a second file that declaratively specifies the UI to render, called the UI-SPEC. The API-SPEC maps out LVGL, the UI-SPEC specifies the concrete layout to render.

Notes:
* DO NOT EDIT api_spec.json!
* DO NOT EDIT ./data/lv_def.json!
* Run all tests (`make; cd ./tests; sh run_tests.sh`) to make sure changes are ok, 1-2 test failures are ok and will be fixed by repo owner afterwards.
* A semi-informal specification of the UI-SPEC format is in the file "./docs/ui_spec.md".
* The app has two "code" generators, c_code that outputs C-Code and lvgl_ui that opens an SDL window and visually renders the UI. The mode is selected via the "--codegen" cmd line switch, options are "c_code", "lvgl_ui", "c_code_and_lvgl_ui".
* A command line application such as an AGENT, will most likely always want `--codegen c_code` as a flag to `lvgl_ui_generator` or the application will time out waiting for user input.

# Code

## Debug statements

If you need to add debug statements, always use `_dprintf(stderr, ...);` instead of `fprintf(stderr, ...)` and enable them by defining `#define __DEBUG=1`.

# Tests

## Test format

Tests are a simple .json file that specifies the UI-SPEC, they are always run against "./api_spec.json", and a .expected file that captures the expected output of running "lvgl_ui_generator" against the UI-SPEC. The .json and .expected files both have the same filename "stem" (ie they're the same filename sans extension).

So currently, the .expected file is C-Code that corresponds to what the codegen backend of "lvgl_ui_generator" outputs when running against a specific UI-SPEC file. So note that the .expected files are not proper C programs that represent a text when run. They are simply the target to diff the output og "lvgl_ui_generator" against. They're text-files representing the output of "lvgl_ui_generator" so to speak - not proper C-programs.

## Running all tests

`make; cd ./tests; sh run_tests.sh`

## Running a single test

`export TESTNAME=any_test_name_here; cd ./tests; sh run_tests.sh #{TESTNAME}


## Test "debug" mode

`run_tests.sh` accepts a "-d" ("--debug") flag that will also output the diff between the actual output and the generated output.

## Regenerating test expected output

If a test input or the generated output C-Code have changed the test's ".expected" file needs to be regenerated after manually ensuring that the output is valid C-Code and is correct.

`export TESTNAME=any_test_name_here; cd ./tests; ../lvgl_ui_generator ../api_spec.json ${TESTNAME}.json > ./tests/${TESTNAME}.expected --codegen c_code 2>&1`

# Tasks

## Building

Makefile based:

`make`

## Running

The main executable is "./lvgl_ui_generator". It takes two arguments, first arg is filename of API-SPEC file (usually always the same when using same LVGL version), second argument is UI-SPEC to render (different for different UIs).

Build first, then:

`./lvgl_ui_generator <api_spec> <ui_spec>`, for example: `./lvgl_ui_generator api_spec.json ui.json`.


## Generating api_spec.json

NOTE: DO NOT EDIT api_spec.json!
NOTE: DO NOT EDIT ./data/lv_def.json!

Generally there's no need to regenerate this file unless upgrading LVGL versions or the file has been corruped. The "api_spec.json" file is auto-generated by `generate_api_spec.py`. "./data/lv_def.json" is the ouput of lvgl `gen_json.py, see https://docs.lvgl.io/master/details/integration/bindings/api_json.html .",

To regenerate api_spec.json:

`python3 generate_api_spec.py lv_def.json > api_spec.json`
