#!/bin/bash
#

sudo WESTON_MODULE_MAP='drm-backend.so=/Volumes/Bedtime/Developer/myland/build-weston/libweston/backend-drm/drm-backend.dylib' \
    /Volumes/Bedtime/Developer/myland/build-weston/frontend/weston --backend=drm
