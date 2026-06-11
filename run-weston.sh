#!/bin/bash
#
# Run weston with terminal on macOS via our DRM/GBM/EGL shims.
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build-weston"
DYLD_INSERT_LIBRARIES="$SCRIPT_DIR/build/libwayland-mac.dylib"
WESTON_DATA_DIR="$SCRIPT_DIR/weston/data"

WESTON_MODULE_MAP="drm-backend.so=${BUILD_DIR}/libweston/backend-drm/drm-backend.dylib"
WESTON_MODULE_MAP="${WESTON_MODULE_MAP};gl-renderer.so=${BUILD_DIR}/libweston/renderer-gl/gl-renderer.dylib"
WESTON_MODULE_MAP="${WESTON_MODULE_MAP};desktop-shell.so=${BUILD_DIR}/desktop-shell/desktop-shell.dylib"
WESTON_MODULE_MAP="${WESTON_MODULE_MAP};weston-keyboard=${BUILD_DIR}/clients/weston-keyboard"
WESTON_MODULE_MAP="${WESTON_MODULE_MAP};weston-desktop-shell=${BUILD_DIR}/clients/weston-desktop-shell"
WESTON_MODULE_MAP="${WESTON_MODULE_MAP};weston-terminal=${BUILD_DIR}/clients/weston-terminal"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/weston-runtime}"
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null

# Kill stale processes
echo "=== Cleaning up ==="
sudo pkill -9 weston 2>/dev/null; sudo pkill -9 framebufferd 2>/dev/null
sudo pkill -9 amfiexceptiond 2>/dev/null; sudo pkill -9 weston-terminal 2>/dev/null
rm -f "$XDG_RUNTIME_DIR"/wayland-*

# Start weston
echo "=== Starting weston ==="
sudo env \
    DYLD_INSERT_LIBRARIES="$DYLD_INSERT_LIBRARIES" \
    WESTON_MODULE_MAP="$WESTON_MODULE_MAP" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    WESTON_DATA_DIR="$WESTON_DATA_DIR" \
    "$BUILD_DIR/frontend/weston" --backend=drm \
    --continue-without-input \
    --config="$SCRIPT_DIR/weston.ini" &

WESTON_PID=$!

# Wait and launch terminal
echo "=== Waiting for wayland socket ==="
TERMINAL_PID=""
for i in $(seq 1 30); do
    SOCK=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v lock | head -1)
    if [ -n "$SOCK" ] && [ -S "$SOCK" ]; then
        DNAME=$(basename "$SOCK")
        echo "Socket: $SOCK"
        echo "=== Launching terminal ==="
        # No DYLD_INSERT_LIBRARIES — terminal uses pure Wayland+cairo (no DRM/GBM/EGL)
sudo env \
            WESTON_DATA_DIR="$WESTON_DATA_DIR" \
            XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
            WAYLAND_DISPLAY="$DNAME" \
            "$BUILD_DIR/clients/weston-terminal" --shell="$SCRIPT_DIR/run/terminal-wrapper.sh" \
            2>/tmp/weston-terminal-err.log &
        TERMINAL_PID=$!
        echo "Terminal PID: $TERMINAL_PID"
        sleep 2
        ps aux | grep weston-terminal | grep -v grep
        echo "=== Terminal stderr ==="
        cat /tmp/weston-terminal-err.log 2>/dev/null || echo "(empty)"
        break
    fi
    sleep 1
done

[ -z "$TERMINAL_PID" ] && echo "ERROR: no wayland socket found"

echo "=== Running (PID $WESTON_PID). Ctrl+C to stop. ==="
trap "kill $WESTON_PID $TERMINAL_PID 2>/dev/null; echo 'Stopped.'; exit" INT TERM
wait $WESTON_PID
[ -n "$TERMINAL_PID" ] && kill "$TERMINAL_PID" 2>/dev/null
