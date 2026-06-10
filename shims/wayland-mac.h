/*
 * wayland-mac.h — single include for libwayland-mac consumers.
 *
 * Pulls in all shim APIs:
 *   - epoll / eventfd / signalfd / timerfd  (epoll-shim)
 *   - Linux DRM / KMS                       (drm shim)
 *   - Internal Mach IPC send helper         (drm_send_json)
 *
 * Future shims should add their umbrella header below.
 */

#ifndef WAYLAND_MAC_H
#define WAYLAND_MAC_H

/* ── epoll shim ───────────────────────────────────────────────────────── */
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

/* ── DRM / KMS shim ───────────────────────────────────────────────────── */
#include <drm_linux.h>   /* Linux-compatible KMS/DRM API */
#include <drm.h>         /* drm_send_json / drm_hello    */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * wayland_mac_init() — no-op placeholder; the real initialisation happens
 * in the dylib constructor (wayland_mac_load).  Call this if you want to
 * ensure the constructor has run before proceeding.
 */
void wayland_mac_init(void);

#ifdef __cplusplus
}
#endif

#endif /* WAYLAND_MAC_H */
