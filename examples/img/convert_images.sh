#!/bin/bash

# BIN FILES.

# Requires icu_tool: https://crates.io/crates/icu_tool
# * brew install W-Mai/homebrew-cellar/icu_tool
# * powershell -c "irm https://github.com/W-Mai/icu/releases/latest/download/icu_tool-installer.ps1 | iex"
# * curl --proto '=https' --tlsv1.2 -LsSf https://github.com/W-Mai/icu/releases/latest/download/icu_tool-installer.sh | sh
# COLOR_FMT=rgb565
#for FILE in `ls *.png`; do icu convert $FILE -F lvgl -C "$COLOR_FMT"; done

# C Files.

# Requires https://github.com/lvgl/lvgl/blob/master/scripts/LVGLImage.py

for FILE in `ls *.png`; do
  BASE=`basename $FILE`
  NAME="img_${BASE%.*}"
  python3 lvgl_image_conv.py --cf RGB565 --rgb565dither --name "$NAME" -o out --ofmt C "$FILE";
done

