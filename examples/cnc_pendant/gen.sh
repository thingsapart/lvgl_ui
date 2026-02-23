#!/bin/sh
../../lvgl_ui_generator ../../api_spec.json cnc_pendant.yaml --codegen c_code > ../../../esp32_visual_pendant/firmware/cnc_interface/src/ui_gen/ui.c
