#!/bin/bash

# Script to build LVGL library

# Exit immediately if a command exits with a non-zero status.
set -e

# Define the LVGL directory relative to this script
LVGL_DIR="$(pwd)/lvgl" # Assumes script is run from project root
LVGL_BUILD_DIR="${LVGL_DIR}/build"
LV_CONF_PATH="${LVGL_DIR}/../viewer/lv_conf.h" # Path to our custom lv_conf.h

# SDL
SDL_INCLUDES="$(pkg-config sdl2 -cflags-only-I)"
SDL_LIBS="$(pkg-config sdl2 -libs)"

echo "LVGL directory: ${LVGL_DIR}"
echo "LVGL build directory: ${LVGL_BUILD_DIR}"
echo "LV_CONF_PATH: ${LV_CONF_PATH}"

# Check if lv_conf.h exists
if [ ! -f "${LV_CONF_PATH}" ]; then
    echo "Error: lv_conf.h not found at ${LV_CONF_PATH}"
    exit 1
fi

# Create build directory if it doesn't exist
mkdir -p "${LVGL_BUILD_DIR}"

# Navigate to LVGL directory to run CMake from there, as LVGL's CMakeLists.txt expects this
cd "${LVGL_DIR}"

echo "Configuring LVGL with CMake..."
# Configure LVGL build with CMake
# We pass LV_CONF_PATH directly to ensure it's used.
# LVGL's CMake system should pick up lv_conf.h if it's in the lvgl root,
# but explicitly setting CMAKE_C_FLAGS to include its directory ensures it.
# However, a more robust way for LVGL CMake is to set LV_CONF_INCLUDE_SIMPLE and add its dir.
# Forcing via CMAKE_C_FLAGS can sometimes be overridden.
# LVGL CMakeLists.txt looks for LV_CONF_PATH as a CMake variable.
cmake -S . -B "${LVGL_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DLV_CONF_PATH="${LV_CONF_PATH}" \
    -DLV_BUILD_CONF_PATH="${LV_CONF_PATH}" \
    -DLV_USE_DEMOS=OFF \
    -DLV_USE_EXAMPLES=OFF \
    -DLV_BUILD_EXAMPLES=OFF \
    -DLV_BUILD_TESTS=OFF \
    -DCMAKE_C_COMPILER=gcc \
    -DCMAKE_C_FLAGS="${SDL_INCLUDES} -I$(pwd)/viewer" \
    -DCMAKE_LD_FLAGS="${SDL_LIBS}"

echo "Building LVGL library..."
# Build the library
cmake --build "${LVGL_BUILD_DIR}" -j$(nproc)

echo "LVGL build complete. Artifacts in ${LVGL_BUILD_DIR}"

echo "Generating lv_def.json..."
python3 ./scripts/gen_json/gen_json.py --lvgl-config="${LV_CONF_PATH}" > ../data/lv_def.json
echo "lv_def.json created. Artifacts in ../data"

# Return to the original directory
cd - > /dev/null

# As a final check, list the contents of the build directory, specifically looking for the static library
echo "Contents of ${LVGL_BUILD_DIR}/lib:"
ls -l "${LVGL_BUILD_DIR}/lib" || echo "lib directory not found or empty."

echo "build_lvgl.sh finished."


