#ifndef XF86DRM_H
#define XF86DRM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int  drm_handle_t;
typedef unsigned int  drm_context_t;
typedef unsigned int  drm_drawable_t;
typedef unsigned long drm_magic_t;

#define DRM_MAX_MINOR   16
#define DRM_NR_MINORS   128

#define DRM_CLOEXEC  0x80000000
#define DRM_RDWR     0x40000000

int drmOpen(const char *name, const char *busid);
int drmOpenWithType(const char *name, const char *busid, int type);
int drmClose(int fd);

int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmSetMaster(int fd);
int drmDropMaster(int fd);
int drmGetMagic(int fd, drm_magic_t *magic);
int drmAuthMagic(int fd, drm_magic_t magic);

int drmIoctl(int fd, unsigned long request, void *arg);

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

int drmSetClientCap(int fd, uint64_t capability, uint64_t value);

#define DRM_CAP_DUMB_BUFFER          0x1
#define DRM_CAP_VBLANK_HIGH_CRTC     0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW   0x4
#define DRM_CAP_PRIME                0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_ASYNC_PAGE_FLIP      0x7
#define DRM_CAP_CURSOR_WIDTH         0x8
#define DRM_CAP_CURSOR_HEIGHT        0x9
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_CAP_PAGE_FLIP_TARGET     0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12
#define DRM_CAP_SYNCOBJ             0x13

#define DRM_CLIENT_CAP_STEREO_3D         1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES  2
#define DRM_CLIENT_CAP_ATOMIC            3
#define DRM_CLIENT_CAP_ASPECT_RATIO      4
#define DRM_CLIENT_CAP_WRITEBACK_CONNECTORS 5

#ifdef __cplusplus
}
#endif

#endif /* XF86DRM_H */
