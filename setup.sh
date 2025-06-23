#/bin/sh
git submodule update --init --depth 1
cd cJSON; make
cd -
sh ./build_lvgl.sh
sh ./make_lvgl_dispatchers.sh
