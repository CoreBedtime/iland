#include "drm.h"
#include "drm_ipc.h"
#include "sdl_backend.h"

#include <IOSurface/IOSurface.h>
#include <stdio.h>
#include <string.h>

// In single-library SDL mode, DRM IPC is replaced by direct in-process presentation.
// The Mach IPC path is removed; we directly present IOSurfaces via SDL.

int drm_send_json(const char *json)
{
    (void)json;
    // No-op in SDL mode – cursor events etc are ignored (weston uses software cursor)
    // Could log for debugging:
    // fprintf(stderr, "[drm] drm_send_json: %s\n", json);
    return 0;
}

int drm_send_json_with_surface(const char *json, mach_port_t surface_port)
{
    (void)json;
    if (surface_port == MACH_PORT_NULL) {
        return drm_send_json(json);
    }

    // Lookup IOSurface from mach port and present via SDL
    IOSurfaceRef surf = IOSurfaceLookupFromMachPort(surface_port);
    if (surf) {
        sdl_present_iosurface(surf);
        CFRelease(surf);
    } else {
        fprintf(stderr, "[drm] IOSurfaceLookupFromMachPort failed for port %d json=%s\n",
                surface_port, json ? json : "(null)");
        // Still succeed to avoid breaking compositor
    }
    return 0;
}
