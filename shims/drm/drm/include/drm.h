#ifndef DRM_H
#define DRM_H

#include <stddef.h>

/* Legacy hello */
void drm_hello(void);

/* Send a JSON string to framebufferd over Mach IPC.
 * Returns 0 on success, -1 on error. */
int drm_send_json(const char *json);

#endif
