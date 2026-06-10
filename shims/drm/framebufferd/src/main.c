#include "drm_ipc.h"

#include <bootstrap.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

/* bootstrap_register is deprecated but remains the only option for
 * ad-hoc daemons not managed by launchd. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

int main(void) {
    /* Register service with bootstrap so drm can look it up */
    mach_port_t server_port = MACH_PORT_NULL;
    kern_return_t kr = mach_port_allocate(mach_task_self(),
                                          MACH_PORT_RIGHT_RECEIVE,
                                          &server_port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[framebufferd] mach_port_allocate: %s\n",
                mach_error_string(kr));
        return 1;
    }

    kr = mach_port_insert_right(mach_task_self(), server_port, server_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[framebufferd] mach_port_insert_right: %s\n",
                mach_error_string(kr));
        return 1;
    }

    kr = bootstrap_register(bootstrap_port, DRM_IPC_SERVICE_NAME, server_port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[framebufferd] bootstrap_register %s: %s\n",
                DRM_IPC_SERVICE_NAME, mach_error_string(kr));
        return 1;
    }

    printf("[framebufferd] listening on %s\n", DRM_IPC_SERVICE_NAME);
    fflush(stdout);

    /* Message server loop */
    for (;;) {
        drm_ipc_msg_t msg = {0};
        msg.header.msgh_size        = sizeof(msg);
        msg.header.msgh_local_port  = server_port;

        kr = mach_msg(&msg.header,
                      MACH_RCV_MSG,
                      0,
                      sizeof(msg),
                      server_port,
                      MACH_MSG_TIMEOUT_NONE,
                      MACH_PORT_NULL);

        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "[framebufferd] mach_msg recv: %s\n",
                    mach_error_string(kr));
            continue;
        }

        if (msg.header.msgh_id != DRM_IPC_MSG_ID) {
            fprintf(stderr, "[framebufferd] unknown msg id 0x%x\n",
                    msg.header.msgh_id);
            continue;
        }

        /* NUL-terminate and print */
        uint32_t len = msg.json_len < DRM_IPC_JSON_MAX
                     ? msg.json_len : DRM_IPC_JSON_MAX - 1;
        msg.json[len] = '\0';
        printf("[framebufferd] %s\n", msg.json);
        fflush(stdout);
    }

    return 0;
}
