#/bin/sh

# Set python path if not passed or defined in env.
PYTHON_PATH=${PYTHON_PATH:-python3.13}

# Generate the api_spec.json. (part of Makefile now)
#${PYTHON_PATH} generate_api_spec.py ./data/lv_def.json > api_spec_full.json

# Run a sample ui.json file.
make && ./lvgl_ui_generator ./api_spec.json ./ui.json
