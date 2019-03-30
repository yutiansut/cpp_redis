#!/bin/sh

find sources includes tests examples -maxdepth 4 \( -name '*.cpp' -o -name '*.hpp' -o -name '*.ipp' -o -name '*.c' -o -name '*.h' \) -exec clang-format -style=file -i {} ';'
