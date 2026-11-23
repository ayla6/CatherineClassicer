#!/bin/sh
# Builds mg.dll (32-bit) inside the "fedora" distrobox (needs mingw32-gcc).
cd "$(dirname "$0")"
exec distrobox enter fedora -- i686-w64-mingw32-gcc -O2 -Wall -Wno-unused-function -shared -o mg.dll mg.c \
  -Wl,--kill-at -static-libgcc -lmfplat -lmfuuid -lole32 -luuid
