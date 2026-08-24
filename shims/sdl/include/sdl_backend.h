#pragma once

#include <IOSurface/IOSurface.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise SDL window/renderer. Idempotent, thread-safe.
// Must be called before sdl_present_iosurface. Can be called from any thread;
// actual SDL init will be dispatched to main thread if needed.
void sdl_backend_init(void);

// Present an IOSurface in the SDL window. Thread-safe.
// The surface is retained internally and may be released by caller immediately.
// If SDL not yet inited, this will trigger lazy init.
void sdl_present_iosurface(IOSurfaceRef surface);
void sdl_present_iosurface_for_crtc(IOSurfaceRef surface, uint32_t crtc_id, int x, int y);

// Shutdown SDL backend.
void sdl_backend_shutdown(void);

// Poll-driven input: called from libinput's thread to ensure events are pumped.
// Optional - SDL backend runs its own poll thread, so libinput need not call this.
void sdl_backend_pump_events(void);

// Expose underlying NSWindow* (AppKit) for direct manipulation.
// Returns NULL if SDL not yet initialized. Valid only on main thread.
void *sdl_get_nswindow(void);

// Shared, stable macOS display geometry (single source of truth).
// Index 0 is the main display. Used both by the virtual DRM mode table and
// by the SDL backend for per-CRTC window placement.
int  mac_display_count(void);
void mac_display_bounds(int idx, int *x, int *y, int *w, int *h);

#ifdef __cplusplus
}
#endif
