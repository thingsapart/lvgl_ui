#!/bin/bash
# Merge generated LVGL api spec with optional custom spec.
# If `api_spec_custom.json` exists, merge it into `api_spec.json`.
# Otherwise copy the generated `api_spec_lvgl.json` to `api_spec.json`.

set -euo pipefail

LVGL_SPEC="api_spec_lvgl.json"
CUSTOM_SPEC="api_spec_custom.json"
OUT_SPEC="api_spec.json"

if [ ! -f "$LVGL_SPEC" ]; then
	echo "Error: generated LVGL api spec '$LVGL_SPEC' not found." >&2
	exit 2
fi

if [ -s "$CUSTOM_SPEC" ]; then
	echo "Merging $LVGL_SPEC + $CUSTOM_SPEC -> $OUT_SPEC"
	python3 json_merge.py "$LVGL_SPEC" "$CUSTOM_SPEC" -o "$OUT_SPEC"
else
	echo "No custom spec found; copying $LVGL_SPEC -> $OUT_SPEC"
	cp "$LVGL_SPEC" "$OUT_SPEC"
fi

exit 0
