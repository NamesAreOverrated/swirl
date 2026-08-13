#!/bin/sh
set -e
meson setup build --buildtype=debug --default-library=static
ninja -C build/
sudo ninja -C build/ install
