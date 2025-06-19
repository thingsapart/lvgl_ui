#/bin/sh

# Set python path if not passed or defined in env.
PYTHON_PATH=${PYTHON_PATH:-python3}

# Generate the api_spec.json.
${PYTHON_PATH} generate_api_spec.py lv_def.json > api_spec.json

# Run a sample ui.json file.
make && ./lvgl_ui_generator ./api_spec.json ./ui.json
