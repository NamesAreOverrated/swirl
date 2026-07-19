#!/bin/sh
set -e
rm -rf build/
meson setup build --buildtype=debug --default-library=static
ninja -C build/
sudo ninja -C build/ install
