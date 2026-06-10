#ifndef DRM_FOURCC_H
#define DRM_FOURCC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_FORMAT_XRGB8888         0x34325258
#define DRM_FORMAT_ARGB8888         0x34325241
#define DRM_FORMAT_XBGR8888         0x34324258
#define DRM_FORMAT_ABGR8888         0x34324241
#define DRM_FORMAT_RGB565           0x36314752
#define DRM_FORMAT_XRGB2101010      0x30335258
#define DRM_FORMAT_ARGB2101010      0x30335241
#define DRM_FORMAT_XBGR2101010      0x30334258
#define DRM_FORMAT_ABGR2101010      0x30334241

#define DRM_FORMAT_MOD_LINEAR       0
#define DRM_FORMAT_MOD_INVALID      ((uint64_t)1 << 56)

#ifdef __cplusplus
}
#endif

#endif /* DRM_FOURCC_H */
