#!/usr/bin/env bash
# Builds libpacing.so: an LD_PRELOAD library that logs every wl_surface.commit and asks the compositor for its wp_presentation feedback.
set -euo pipefail

cd "$(dirname "$0")"

XML=/usr/share/wayland-protocols/stable/presentation-time/presentation-time.xml

wayland-scanner client-header "$XML" presentation-time-client-protocol.h
wayland-scanner private-code "$XML" presentation-time-protocol.c

gcc -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter \
  -o libpacing.so pacing_shim.c presentation-time-protocol.c \
  $(pkg-config --cflags --libs wayland-client) -ldl -lpthread
