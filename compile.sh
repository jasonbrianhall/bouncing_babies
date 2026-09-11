#!/bin/sh
set -e
cd "$(dirname "$0")"

./generate_midi_headers.sh

g++ bouncingbabies.cpp dbopl.cpp dbopl_wrapper.cpp instruments.cpp midi_render.cpp \
    -Igenerated -I. \
    -o bouncing_babies `sdl2-config --cflags --libs` -lSDL2_ttf -std=c++17
