#!/bin/bash
#

BUILD_DIR="/Volumes/Bedtime/Developer/myland/build-weston"
DYLD_INSERT_LIBRARIES="/Volumes/Bedtime/Developer/myland/build/libwayland-mac.dylib"

WESTON_MODULE_MAP="drm-backend.so=${BUILD_DIR}/libweston/backend-drm/drm-backend.dylib"
WESTON_MODULE_MAP="${WESTON_MODULE_MAP};gl-renderer.so=${BUILD_DIR}/libweston/renderer-gl/gl-renderer.dylib"

sudo WESTON_MODULE_MAP="$WESTON_MODULE_MAP" \
    DYLD_INSERT_LIBRARIES="$DYLD_INSERT_LIBRARIES" \
    "$BUILD_DIR/frontend/weston" --backend=drm
