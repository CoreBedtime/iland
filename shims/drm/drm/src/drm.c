#include "drm.h"
#include "drm_ipc.h"

#include <bootstrap.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

static int send_msg(drm_ipc_msg_t *msg, size_t msg_size)
{
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(bootstrap_port,
                                         DRM_IPC_SERVICE_NAME, &port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[drm] bootstrap_look_up %s: %s\n",
                DRM_IPC_SERVICE_NAME, mach_error_string(kr));
        return -1;
    }

    msg->header.msgh_remote_port = port;
    msg->header.msgh_size        = msg_size;

    fprintf(stderr, "[drm] send: msg_size=%zu sizeof_all=%zu hdr=%zu body=%zu desc=%zu jlen=%zu complex=%d\n",
            msg_size, sizeof(drm_ipc_msg_t), sizeof(msg->header),
            sizeof(msg->body), sizeof(msg->surface_port),
            sizeof(msg->json_len),
            !!(msg->header.msgh_bits & MACH_MSGH_BITS_COMPLEX));

    kr = mach_msg(&msg->header,
                  MACH_SEND_MSG,
                  msg_size,
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

int drm_send_json(const char *json)
{
    if (!json) return -1;

    drm_ipc_msg_t msg = {0};
    size_t len = strlen(json);
    if (len >= DRM_IPC_JSON_MAX) {
        fprintf(stderr, "[drm] json too large (%zu bytes)\n", len);
        return -1;
    }

    msg.header.msgh_bits   = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_remote_port = MACH_PORT_NULL; /* filled by send_msg */
    msg.header.msgh_local_port  = MACH_PORT_NULL;
    msg.header.msgh_id          = DRM_IPC_MSG_ID;
    msg.body.msgh_descriptor_count = 0;

    msg.json_len = (uint32_t)len;
    memcpy(msg.json, json, len);

    /* Compute message size as fixed struct prefix up to (but not including)
     * the json array, plus the actual json data.  Using sizeof captures
     * any compiler-inserted padding between struct fields; manually
     * summing individual field sizes may undercount if new fields or
     * alignment padding is added. */
    size_t msg_size = sizeof(drm_ipc_msg_t) - DRM_IPC_JSON_MAX + len;
    return send_msg(&msg, msg_size);
}

int drm_send_json_with_surface(const char *json, mach_port_t surface_port)
{
    if (!json) return -1;

    drm_ipc_msg_t msg = {0};
    size_t len = strlen(json);
    if (len >= DRM_IPC_JSON_MAX) {
        fprintf(stderr, "[drm] json too large (%zu bytes)\n", len);
        return -1;
    }

    if (surface_port == MACH_PORT_NULL)
        return drm_send_json(json);

    /* Build complex message with a port descriptor for the IOSurface */
    msg.header.msgh_bits = MACH_MSGH_BITS_COMPLEX
                         | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_remote_port = MACH_PORT_NULL;
    msg.header.msgh_local_port  = MACH_PORT_NULL;
    msg.header.msgh_id          = DRM_IPC_MSG_ID;

    msg.body.msgh_descriptor_count = 1;
    msg.surface_port.name        = surface_port;
    msg.surface_port.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.surface_port.type        = MACH_MSG_PORT_DESCRIPTOR;

    msg.json_len = (uint32_t)len;
    memcpy(msg.json, json, len);

    size_t msg_size = sizeof(drm_ipc_msg_t) - DRM_IPC_JSON_MAX + len;
    return send_msg(&msg, msg_size);
}
