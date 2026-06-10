#ifndef DRM_IPC_H
#define DRM_IPC_H

#include <mach/mach.h>
#include <stdint.h>

#define DRM_IPC_SERVICE_NAME  "com.wayland-mac.framebufferd"
#define DRM_IPC_MSG_ID        0x44524D31   /* 'DRM1' */
#define DRM_IPC_JSON_MAX      4096

typedef struct {
    mach_msg_header_t header;
    uint32_t          json_len;
    char              json[DRM_IPC_JSON_MAX];
} drm_ipc_msg_t;

#endif /* DRM_IPC_H */
