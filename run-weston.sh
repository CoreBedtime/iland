#!/bin/bash
#

BUILD_DIR="/Volumes/Bedtime/Developer/myland/build-weston"
DYLD_INSERT_LIBRARIES="/Volumes/Bedtime/Developer/myland/build/libwayland-mac.dylib"

WESTON_MODULE_MAP="drm-backend.so=${BUILD_DIR}/libweston/backend-drm/drm-backend.dylib"
WESTON_MODULE_MAP="${WESTON_MODULE_MAP};gl-renderer.so=${BUILD_DIR}/libweston/renderer-gl/gl-renderer.dylib"
WESTON_MODULE_MAP="${WESTON_MODULE_MAP};desktop-shell.so=${BUILD_DIR}/desktop-shell/desktop-shell.dylib"

# XDG_RUNTIME_DIR for Wayland socket
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/weston-runtime}"
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null

sudo env \
    DYLD_INSERT_LIBRARIES="$DYLD_INSERT_LIBRARIES" \
    WESTON_MODULE_MAP="$WESTON_MODULE_MAP" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    "$BUILD_DIR/frontend/weston" --backend=drm \
    --config=/Volumes/Bedtime/Developer/myland/weston.ini
