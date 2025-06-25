#/bin/sh

# Init git submodules.
git submodule update --init --depth 1

# Install Linux packages.
if command -v apt-get &>/dev/null; then
  sudo apt-get update && sudo apt-get install -y libsdl2-dev doxygen
fi

# Install python deps.
pip install pycparser

# Build.
cd cJSON; make
cd -
sh ./build_lvgl.sh
sh ./make_lvgl_dispatchers.sh
