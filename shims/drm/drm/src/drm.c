#include "drm.h"
#include "drm_ipc.h"

#include <bootstrap.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

void drm_hello(void) {
    printf("hello from drm\n");
}

int drm_send_json(const char *json) {
    if (!json) return -1;

    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(bootstrap_port,
                                         DRM_IPC_SERVICE_NAME, &port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[drm] bootstrap_look_up %s: %s\n",
                DRM_IPC_SERVICE_NAME, mach_error_string(kr));
        return -1;
    }

    drm_ipc_msg_t msg = {0};
    msg.header.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_remote_port = port;
    msg.header.msgh_local_port  = MACH_PORT_NULL;
    msg.header.msgh_id          = DRM_IPC_MSG_ID;
    msg.header.msgh_size        = sizeof(msg);

    size_t len = strlen(json);
    if (len >= DRM_IPC_JSON_MAX) {
        fprintf(stderr, "[drm] json too large (%zu bytes)\n", len);
        mach_port_deallocate(mach_task_self(), port);
        return -1;
    }
    msg.json_len = (uint32_t)len;
    memcpy(msg.json, json, len);

    kr = mach_msg(&msg.header,
                  MACH_SEND_MSG,
                  sizeof(msg),
                  0,
                  MACH_PORT_NULL,
                  MACH_MSG_TIMEOUT_NONE,
                  MACH_PORT_NULL);

    mach_port_deallocate(mach_task_self(), port);

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[drm] mach_msg send: %s\n", mach_error_string(kr));
        return -1;
    }
    return 0;
}
