#include <egl_shim.h>
#include <gbm_priv.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOSurface/IOSurface.h>

static void *g_angle_handle = NULL;

#define ANGLE_FN(name) static __typeof__(&name) real_##name = NULL

/* Bit used to mark duplicate EGLConfigs that report XRGB8888 */
#define XRGB_DUP_BIT ((EGLConfig)(uintptr_t)0x80000000)

ANGLE_FN(eglGetDisplay);
ANGLE_FN(eglInitialize);
ANGLE_FN(eglTerminate);
ANGLE_FN(eglGetError);
ANGLE_FN(eglQueryString);
ANGLE_FN(eglGetConfigs);
ANGLE_FN(eglChooseConfig);
ANGLE_FN(eglGetConfigAttrib);
ANGLE_FN(eglCreateContext);
ANGLE_FN(eglDestroyContext);
ANGLE_FN(eglCreateWindowSurface);
ANGLE_FN(eglDestroySurface);
ANGLE_FN(eglMakeCurrent);
ANGLE_FN(eglSwapBuffers);
ANGLE_FN(eglBindAPI);
ANGLE_FN(eglWaitGL);
ANGLE_FN(eglSwapInterval);
ANGLE_FN(eglCreatePbufferSurface);

static void (*g_glReadPixels)(int, int, int, int, unsigned int, unsigned int, void *) = NULL;

/* Thread-local reusable pixel buffer to avoid malloc/free per frame */
static __thread void  *g_pixels    = NULL;
static __thread size_t g_pixels_sz = 0;

static inline uint32_t rgba_to_bgra(uint32_t rgba)
{
    return (rgba & 0xFF00FF00u) | ((rgba >> 16) & 0xFFu) | ((rgba & 0xFFu) << 16);
}

static int load_angle(void)
{
    if (g_angle_handle) return 0;
    g_angle_handle = dlopen("/opt/local/lib/libEGL.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!g_angle_handle) return -1;

#define LOAD(name) do { \
    real_##name = dlsym(g_angle_handle, #name); \
    if (!real_##name) return -1; \
} while(0)

    LOAD(eglGetDisplay);
    LOAD(eglInitialize);
    LOAD(eglTerminate);
    LOAD(eglGetError);
    LOAD(eglQueryString);
    LOAD(eglGetConfigs);
    LOAD(eglChooseConfig);
    LOAD(eglGetConfigAttrib);
    LOAD(eglCreateContext);
    LOAD(eglDestroyContext);
    LOAD(eglCreateWindowSurface);
    LOAD(eglDestroySurface);
    LOAD(eglMakeCurrent);
    LOAD(eglSwapBuffers);
    LOAD(eglBindAPI);
    LOAD(eglWaitGL);
    LOAD(eglSwapInterval);
    LOAD(eglCreatePbufferSurface);

    return 0;
}

static void load_gles2(void)
{
    if (g_glReadPixels) return;
    void *h = dlopen("/opt/local/lib/libGLESv2.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return;
    g_glReadPixels = dlsym(h, "glReadPixels");
}

static EGLShimDisplay *unwrap_display(EGLDisplay dpy)
{
    return (EGLShimDisplay *)dpy;
}

static EGLShimSurface *unwrap_surface(EGLSurface surf)
{
    return (EGLShimSurface *)surf;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType display_id)
{
    if (load_angle() < 0) return EGL_NO_DISPLAY;

    EGLShimDisplay *dpy = calloc(1, sizeof(*dpy));
    if (!dpy) return EGL_NO_DISPLAY;

    dpy->gbm_device = (struct gbm_device *)display_id;
    dpy->angle_display = real_eglGetDisplay(EGL_DEFAULT_DISPLAY);

    return (EGLDisplay)dpy;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    load_gles2();
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglInitialize(dpy, major, minor);
    return real_eglInitialize(sd->angle_display, major, minor);
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglTerminate(dpy);
    EGLBoolean ret = real_eglTerminate(sd->angle_display);
    if (g_pixels) { free(g_pixels); g_pixels = NULL; g_pixels_sz = 0; }
    free(sd);
    return ret;
}

EGLint eglGetError(void)
{
    if (!real_eglGetError) return EGL_SUCCESS;
    return real_eglGetError();
}

const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    if (dpy == EGL_NO_DISPLAY) return NULL;
    if (!real_eglQueryString) return NULL;
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglQueryString(dpy, name);
    return real_eglQueryString(sd->angle_display, name);
}

EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *configs,
                          EGLint config_size, EGLint *num_config)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglGetConfigs(dpy, configs, config_size, num_config);
    return real_eglGetConfigs(sd->angle_display, configs, config_size, num_config);
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                            EGLConfig *configs, EGLint config_size,
                            EGLint *num_config)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglChooseConfig(dpy, attrib_list, configs,
                                          config_size, num_config);

    int n_attribs = 0;
    if (attrib_list) {
        const EGLint *p = attrib_list;
        while (p[0] != EGL_NONE) { n_attribs += 2; p += 2; }
        n_attribs += 1;
    }

    EGLint *mod_attribs = malloc((n_attribs + 1) * sizeof(EGLint));
    if (!mod_attribs) return EGL_FALSE;

    if (attrib_list) {
        memcpy(mod_attribs, attrib_list, (n_attribs + 1) * sizeof(EGLint));
        for (int i = 0; mod_attribs[i] != EGL_NONE; i += 2) {
            if (mod_attribs[i] == EGL_SURFACE_TYPE) {
                mod_attribs[i + 1] = EGL_PBUFFER_BIT;
                break;
            }
        }
    }

    EGLBoolean ret = real_eglChooseConfig(sd->angle_display, mod_attribs,
                                           configs, config_size, num_config);
    free(mod_attribs);
    return ret;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                               EGLint attribute, EGLint *value)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglGetConfigAttrib(dpy, config, attribute, value);

    if (attribute == EGL_SURFACE_TYPE) {
        if (!real_eglGetConfigAttrib(sd->angle_display, config,
                                      attribute, value))
            return EGL_FALSE;
        if (*value & EGL_PBUFFER_BIT)
            *value |= EGL_WINDOW_BIT;
        return EGL_TRUE;
    }

    if (attribute == EGL_NATIVE_VISUAL_ID) {
        static const EGLint rgba_attrs[] = {
            EGL_ALPHA_SIZE, EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE
        };
        EGLint rgba[4];
        for (int i = 0; i < 4; i++) {
            if (!real_eglGetConfigAttrib(sd->angle_display, config,
                                          rgba_attrs[i], &rgba[i]))
                return EGL_FALSE;
        }
        if (rgba[1] == 8 && rgba[2] == 8 && rgba[3] == 8) {
            *value = rgba[0] == 8 ? 0x34325241  /* AR24 → ARGB8888 */
                                  : 0x34325258; /* XR24 → XRGB8888 */
            return EGL_TRUE;
        }
    }

    return real_eglGetConfigAttrib(sd->angle_display, config, attribute, value);
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                             EGLContext share_context,
                             const EGLint *attrib_list)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglCreateContext(dpy, config, share_context, attrib_list);
    return real_eglCreateContext(sd->angle_display, config, share_context, attrib_list);
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglDestroyContext(dpy, ctx);
    return real_eglDestroyContext(sd->angle_display, ctx);
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                   EGLNativeWindowType win,
                                   const EGLint *attrib_list)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglCreateWindowSurface(dpy, config, win, attrib_list);

    struct gbm_surface *gs = (struct gbm_surface *)win;
    if (!gs) return EGL_NO_SURFACE;

    EGLShimSurface *ss = calloc(1, sizeof(*ss));
    if (!ss) return EGL_NO_SURFACE;

    ss->gbm_surface = gs;
    ss->width  = gbm_bo_get_width(gs->bos[0]);
    ss->height = gbm_bo_get_height(gs->bos[0]);

    EGLint pb_attribs[] = {
        EGL_WIDTH,  (EGLint)ss->width,
        EGL_HEIGHT, (EGLint)ss->height,
        EGL_NONE
    };

    ss->angle_surface = real_eglCreatePbufferSurface(sd->angle_display,
                                                       config, pb_attribs);
    if (!ss->angle_surface) {
        free(ss);
        return EGL_NO_SURFACE;
    }

    return (EGLSurface)ss;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglDestroySurface(dpy, surface);

    EGLShimSurface *ss = unwrap_surface(surface);
    if (!ss) return real_eglDestroySurface(dpy, surface);

    real_eglDestroySurface(sd->angle_display, ss->angle_surface);
    free(ss);
    return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
                           EGLSurface read, EGLContext ctx)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglMakeCurrent(dpy, draw, read, ctx);

    EGLShimSurface *sdraw = unwrap_surface(draw);
    EGLShimSurface *sread = unwrap_surface(read);

    EGLSurface adraw = sdraw ? sdraw->angle_surface : draw;
    EGLSurface aread = sread ? sread->angle_surface : read;

    return real_eglMakeCurrent(sd->angle_display, adraw, aread, ctx);
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglSwapBuffers(dpy, surface);

    EGLShimSurface *ss = unwrap_surface(surface);
    if (!ss) return real_eglSwapBuffers(dpy, surface);

    struct gbm_surface *gs = ss->gbm_surface;
    struct gbm_bo *bo = gbm_surface_get_write_bo(gs);
    IOSurfaceRef iosurf = gbm_bo_get_iosurface(bo);

    uint32_t w = ss->width;
    uint32_t h = ss->height;
    size_t total = (size_t)w * h * 4;

    if (!g_glReadPixels) return real_eglSwapBuffers(sd->angle_display, ss->angle_surface);

    /* Reuse thread-local buffer to avoid malloc/free per frame */
    if (g_pixels_sz < total) {
        void *p = realloc(g_pixels, total);
        if (!p) return real_eglSwapBuffers(sd->angle_display, ss->angle_surface);
        g_pixels = p;
        g_pixels_sz = total;
    }

    /* Read pixels BEFORE swap — back buffer content is undefined after */
    g_glReadPixels(0, 0, (int)w, (int)h, 0x1908, 0x1401, g_pixels);

    EGLBoolean ret = real_eglSwapBuffers(sd->angle_display, ss->angle_surface);
    if (!ret) return ret;

    IOSurfaceLock(iosurf, 0, NULL);
    uint32_t *src32 = (uint32_t *)g_pixels;
    uint32_t *dst32 = (uint32_t *)IOSurfaceGetBaseAddress(iosurf);
    size_t dst_pitch = IOSurfaceGetBytesPerRow(iosurf) / 4;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *s = src32 + (h - 1 - y) * w;
        uint32_t *d = dst32 + y * dst_pitch;
        for (uint32_t x = 0; x < w; x++)
            d[x] = rgba_to_bgra(s[x]);
    }

    IOSurfaceUnlock(iosurf, 0, NULL);

    gbm_surface_advance_write(gs);

    return EGL_TRUE;
}

EGLBoolean eglBindAPI(EGLenum api)
{
    if (!real_eglBindAPI) return EGL_FALSE;
    return real_eglBindAPI(api);
}

EGLBoolean eglWaitGL(void)
{
    if (!real_eglWaitGL) return EGL_FALSE;
    return real_eglWaitGL();
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    EGLShimDisplay *sd = unwrap_display(dpy);
    if (!sd) return real_eglSwapInterval(dpy, interval);
    return real_eglSwapInterval(sd->angle_display, interval);
}
