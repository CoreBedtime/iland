#ifndef EGL_SHIM_H
#define EGL_SHIM_H

#include <gbm.h>
#include <EGL/egl.h>

typedef struct EGLShimDisplay {
    EGLDisplay angle_display;
    struct gbm_device *gbm_device;
} EGLShimDisplay;

typedef struct EGLShimSurface {
    EGLSurface angle_surface;
    struct gbm_surface *gbm_surface;
    uint32_t width;
    uint32_t height;
} EGLShimSurface;

#endif
