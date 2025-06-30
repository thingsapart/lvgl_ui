#/bin/bash

for file in `ls *.yaml | sort`; do
  ../lvgl_ui_generator ../api_spec.json --codegen ir_debug_print,lvgl_render "$file"
done
