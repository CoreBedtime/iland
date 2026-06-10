#include "drm_linux.h"
#include "drm.h"

#include <IOSurface/IOSurface.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>

#define DRM_VIRTUAL_FD  42

/* ── static mode table ────────────────────────────────────────────────── */

static const drmModeModeInfo g_modes[] = {
    {
        .clock        = 148500,
        .hdisplay     = 1920, .hsync_start = 2008,
        .hsync_end    = 2052, .htotal      = 2200, .hskew = 0,
        .vdisplay     = 1080, .vsync_start = 1084,
        .vsync_end    = 1089, .vtotal      = 1125, .vscan = 0,
        .vrefresh     = 60,
        .flags        = 0,
        .type         = 0,
        .name         = "1920x1080",
    },
    {
        .clock        = 74250,
        .hdisplay     = 1280, .hsync_start = 1390,
        .hsync_end    = 1430, .htotal      = 1650, .hskew = 0,
        .vdisplay     = 720,  .vsync_start = 725,
        .vsync_end    = 730,  .vtotal      = 750,  .vscan = 0,
        .vrefresh     = 60,
        .flags        = 0,
        .type         = 0,
        .name         = "1280x720",
    },
};
#define G_MODE_COUNT  ((int)(sizeof(g_modes) / sizeof(g_modes[0])))

/* active CRTC state */
static struct {
    uint32_t        crtc_fb_id;
    drmModeModeInfo crtc_mode;
    int             crtc_mode_valid;
} g_state;

/* ── helpers ──────────────────────────────────────────────────────────── */

static int check_fd(int fd)
{
    if (fd != DRM_VIRTUAL_FD) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

/* ── open / close ─────────────────────────────────────────────────────── */

int drmOpen(const char *name, const char *busid)
{
    (void)name; (void)busid;
    return DRM_VIRTUAL_FD;
}

int drmOpenWithType(const char *name, const char *busid, int type)
{
    (void)name; (void)busid; (void)type;
    return DRM_VIRTUAL_FD;
}

int drmClose(int fd)
{
    (void)fd;
    return 0;
}

/* ── capability ───────────────────────────────────────────────────────── */

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
    if (check_fd(fd) < 0) return -1;
    if (!value) { errno = EINVAL; return -1; }

    switch (capability) {
    case DRM_CAP_DUMB_BUFFER:         *value = 1; break;
    case DRM_CAP_PRIME:               *value = 3; break;
    case DRM_CAP_TIMESTAMP_MONOTONIC: *value = 1; break;
    case DRM_CAP_ASYNC_PAGE_FLIP:     *value = 0; break;
    case DRM_CAP_CURSOR_WIDTH:        *value = 64; break;
    case DRM_CAP_CURSOR_HEIGHT:       *value = 64; break;
    case DRM_CAP_ADDFB2_MODIFIERS:    *value = 0; break;
    default:                          *value = 0; break;
    }
    return 0;
}

/* ── resources ────────────────────────────────────────────────────────── */

drmModeResPtr drmModeGetResources(int fd)
{
    if (check_fd(fd) < 0) return NULL;

    drmModeRes *r = calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->count_fbs        = 0;
    r->fbs              = NULL;

    r->count_crtcs      = 1;
    r->crtcs            = malloc(sizeof(uint32_t));
    r->crtcs[0]         = 1;

    r->count_connectors = 1;
    r->connectors       = malloc(sizeof(uint32_t));
    r->connectors[0]    = 1;

    r->count_encoders   = 1;
    r->encoders         = malloc(sizeof(uint32_t));
    r->encoders[0]      = 1;

    r->min_width  = 1;    r->max_width  = 8192;
    r->min_height = 1;    r->max_height = 8192;

    return r;
}

void drmModeFreeResources(drmModeResPtr ptr)
{
    if (!ptr) return;
    free(ptr->fbs);
    free(ptr->crtcs);
    free(ptr->connectors);
    free(ptr->encoders);
    free(ptr);
}

/* ── connector ────────────────────────────────────────────────────────── */

drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connector_id)
{
    if (check_fd(fd) < 0) return NULL;
    if (connector_id != 1) { errno = ENOENT; return NULL; }

    drmModeConnector *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->connector_id      = 1;
    c->encoder_id        = 1;
    c->connector_type    = DRM_MODE_CONNECTOR_DisplayPort;
    c->connector_type_id = 1;
    c->connection        = DRM_MODE_CONNECTED;
    c->mmWidth           = 527;
    c->mmHeight          = 296;
    c->subpixel          = 0;

    c->count_modes       = G_MODE_COUNT;
    c->modes             = malloc(G_MODE_COUNT * sizeof(drmModeModeInfo));
    memcpy(c->modes, g_modes, G_MODE_COUNT * sizeof(drmModeModeInfo));

    c->count_props       = 0;
    c->props             = NULL;
    c->prop_values       = NULL;

    c->count_encoders    = 1;
    c->encoders          = malloc(sizeof(uint32_t));
    c->encoders[0]       = 1;

    return c;
}

void drmModeFreeConnector(drmModeConnectorPtr ptr)
{
    if (!ptr) return;
    free(ptr->modes);
    free(ptr->props);
    free(ptr->prop_values);
    free(ptr->encoders);
    free(ptr);
}

/* ── encoder ──────────────────────────────────────────────────────────── */

drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encoder_id)
{
    if (check_fd(fd) < 0) return NULL;
    if (encoder_id != 1) { errno = ENOENT; return NULL; }

    drmModeEncoder *e = calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->encoder_id     = 1;
    e->encoder_type   = 10;
    e->crtc_id        = 1;
    e->possible_crtcs = 0x1;
    e->possible_clones= 0x0;

    return e;
}

void drmModeFreeEncoder(drmModeEncoderPtr ptr)
{
    free(ptr);
}

/* ── CRTC ─────────────────────────────────────────────────────────────── */

drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtc_id)
{
    if (check_fd(fd) < 0) return NULL;
    if (crtc_id != 1) { errno = ENOENT; return NULL; }

    drmModeCrtc *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->crtc_id    = 1;
    c->buffer_id  = g_state.crtc_fb_id;
    c->x = c->y  = 0;
    c->width      = g_state.crtc_mode_valid ? g_state.crtc_mode.hdisplay : 0;
    c->height     = g_state.crtc_mode_valid ? g_state.crtc_mode.vdisplay : 0;
    c->mode_valid = g_state.crtc_mode_valid;
    if (g_state.crtc_mode_valid)
        c->mode = g_state.crtc_mode;
    c->gamma_size = 256;

    return c;
}

void drmModeFreeCrtc(drmModeCrtcPtr ptr)
{
    free(ptr);
}

/* ── IOSurface-backed dumb buffer + framebuffer management ───────────── */

#include <IOSurface/IOSurface.h>

#define MAX_DUMB_BUFS 64
#define MAX_FBS       64

/* A dumb buffer is backed by a real IOSurface */
typedef struct dumb_buf {
    uint32_t    handle;
    IOSurfaceRef surface;
    uint32_t    width;
    uint32_t    height;
    uint32_t    bpp;
    uint32_t    pitch;
    size_t      size;
    void       *map;
} dumb_buf_t;

static dumb_buf_t  g_dumb[MAX_DUMB_BUFS];
static uint32_t    g_next_dumb_handle = 1;

/* A framebuffer wraps a reference to a dumb buffer's IOSurface */
typedef struct fb_entry {
    uint32_t    fb_id;
    uint32_t    handle;       /* dumb buffer handle */
    IOSurfaceRef surface;     /* retained */
    uint32_t    width;
    uint32_t    height;
    uint32_t    bpp;
    uint32_t    pitch;
} fb_entry_t;

static fb_entry_t  g_fbs[MAX_FBS];
static uint32_t    g_next_fb_id = 1;

/* ── dumb buffers ─────────────────────────────────────────────────────── */

int drmModeCreateDumbBuffer(int fd, uint32_t width, uint32_t height,
                            uint32_t bpp, uint32_t flags,
                            uint32_t *handle, uint32_t *pitch,
                            uint64_t *size)
{
    if (check_fd(fd) < 0) return -1;
    (void)flags;

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_DUMB_BUFS; i++) {
        if (g_dumb[i].handle == 0) { slot = i; break; }
    }
    if (slot < 0) { errno = ENOMEM; return -1; }

    uint32_t bpe = (bpp + 7) / 8;
    uint32_t p   = (width * bpe + 63) & ~63u;
    size_t   sz  = (size_t)p * height;

    /* Create a real IOSurface */
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    CFNumberRef num;
    num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &width);
    CFDictionarySetValue(props, kIOSurfaceWidth, num);
    CFRelease(num);

    num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &height);
    CFDictionarySetValue(props, kIOSurfaceHeight, num);
    CFRelease(num);

    num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpe);
    CFDictionarySetValue(props, kIOSurfaceBytesPerElement, num);
    CFRelease(num);

    uint32_t pf = 'BGRA';
    num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pf);
    CFDictionarySetValue(props, kIOSurfacePixelFormat, num);
    CFRelease(num);

    IOSurfaceRef surf = IOSurfaceCreate(props);
    CFRelease(props);

    if (!surf) { errno = ENOMEM; return -1; }

    /* Lock and get base address so the compositor can write pixels */
    IOSurfaceLock(surf, 0, NULL);
    void *base = IOSurfaceGetBaseAddress(surf);
    IOSurfaceUnlock(surf, 0, NULL);

    uint32_t h = g_next_dumb_handle++;
    g_dumb[slot] = (dumb_buf_t){
        .handle = h,
        .surface = surf,
        .width   = width,
        .height  = height,
        .bpp     = bpp,
        .pitch   = p,
        .size    = sz,
        .map     = base,
    };

    if (handle) *handle = h;
    if (pitch)  *pitch  = (uint32_t)p;
    if (size)   *size   = sz;
    return 0;
}

int drmModeDestroyDumbBuffer(int fd, uint32_t handle)
{
    if (check_fd(fd) < 0) return -1;
    for (int i = 0; i < MAX_DUMB_BUFS; i++) {
        if (g_dumb[i].handle == handle) {
            if (g_dumb[i].surface) {
                CFRelease(g_dumb[i].surface);
            }
            g_dumb[i] = (dumb_buf_t){0};
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

int drmModeMapDumbBuffer(int fd, uint32_t handle, uint64_t *offset)
{
    if (check_fd(fd) < 0) return -1;
    for (int i = 0; i < MAX_DUMB_BUFS; i++) {
        if (g_dumb[i].handle == handle) {
            if (offset) *offset = (uint64_t)(uintptr_t)g_dumb[i].map;
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

/* ── framebuffers ─────────────────────────────────────────────────────── */

int drmModeAddFB(int fd, uint32_t width, uint32_t height,
                 uint8_t depth, uint8_t bpp,
                 uint32_t pitch, uint32_t bo_handle,
                 uint32_t *buf_id)
{
    if (check_fd(fd) < 0) return -1;
    if (!buf_id) { errno = EINVAL; return -1; }

    /* Find the dumb buffer backing this handle */
    IOSurfaceRef surf = NULL;
    for (int i = 0; i < MAX_DUMB_BUFS; i++) {
        if (g_dumb[i].handle == bo_handle) {
            surf = g_dumb[i].surface;
            break;
        }
    }
    if (!surf) { errno = ENOENT; return -1; }

    /* Find free FB slot */
    int slot = -1;
    for (int i = 0; i < MAX_FBS; i++) {
        if (g_fbs[i].fb_id == 0) { slot = i; break; }
    }
    if (slot < 0) { errno = ENOMEM; return -1; }

    uint32_t fid = g_next_fb_id++;
    CFRetain(surf);
    g_fbs[slot] = (fb_entry_t){
        .fb_id   = fid,
        .handle  = bo_handle,
        .surface = surf,
        .width   = width,
        .height  = height,
        .bpp     = bpp,
        .pitch   = pitch,
    };

    *buf_id = fid;
    return 0;
}

int drmModeAddFB2(int fd, uint32_t width, uint32_t height,
                  uint32_t pixel_format,
                  const uint32_t bo_handles[4],
                  const uint32_t pitches[4],
                  const uint32_t offsets[4],
                  uint32_t *buf_id, uint32_t flags)
{
    (void)pixel_format; (void)offsets; (void)flags;
    return drmModeAddFB(fd, width, height, 24, 32,
                        pitches ? pitches[0] : 0,
                        bo_handles ? bo_handles[0] : 0,
                        buf_id);
}

int drmModeRmFB(int fd, uint32_t buf_id)
{
    if (check_fd(fd) < 0) return -1;
    for (int i = 0; i < MAX_FBS; i++) {
        if (g_fbs[i].fb_id == buf_id) {
            if (g_fbs[i].surface) CFRelease(g_fbs[i].surface);
            g_fbs[i] = (fb_entry_t){0};
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

/* ── helpers to look up an IOSurface from an fb_id ────────────────────── */

static IOSurfaceRef fb_id_to_surface(uint32_t fb_id)
{
    for (int i = 0; i < MAX_FBS; i++) {
        if (g_fbs[i].fb_id == fb_id) return g_fbs[i].surface;
    }
    return NULL;
}

/* ── mode set + page flip ─────────────────────────────────────────────── */

int drmModeSetCrtc(int fd, uint32_t crtc_id, uint32_t fb_id,
                   uint32_t x, uint32_t y,
                   uint32_t *connectors, int count,
                   drmModeModeInfo *mode)
{
    if (check_fd(fd) < 0) return -1;
    if (crtc_id != 1) { errno = ENOENT; return -1; }
    (void)x; (void)y; (void)connectors; (void)count;

    g_state.crtc_fb_id      = fb_id;
    g_state.crtc_mode_valid = (mode != NULL);
    if (mode) g_state.crtc_mode = *mode;

    /* SetCrtc just records state — the actual surface is sent on page flip */
    return 0;
}

int drmModePageFlip(int fd, uint32_t crtc_id, uint32_t fb_id,
                    uint32_t flags, void *user_data)
{
    if (check_fd(fd) < 0) return -1;
    if (crtc_id != 1) { errno = ENOENT; return -1; }

    g_state.crtc_fb_id = fb_id;

    IOSurfaceRef surf = fb_id_to_surface(fb_id);
    mach_port_t surface_port = MACH_PORT_NULL;
    if (surf) {
        surface_port = IOSurfaceCreateMachPort(surf);
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"op\":\"page_flip\",\"crtc\":%u,\"fb\":%u,\"flags\":%u}",
             crtc_id, fb_id, flags);

    int ret = drm_send_json_with_surface(buf, surface_port);

    if (surface_port != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), surface_port);

    (void)user_data;
    return ret;
}

int drmHandleEvent(int fd, drmEventContextPtr evctx)
{
    (void)fd; (void)evctx;
    return 0;
}

/* ── prime stubs ──────────────────────────────────────────────────────── */

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
    (void)fd; (void)handle; (void)flags;
    if (prime_fd) *prime_fd = -1;
    errno = ENOSYS;
    return -1;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
    (void)fd; (void)prime_fd;
    if (handle) *handle = 0;
    errno = ENOSYS;
    return -1;
}

/* ── generic ioctl ────────────────────────────────────────────────────── */

int drmIoctl(int fd, unsigned long request, void *arg)
{
    (void)fd; (void)request; (void)arg;
    errno = ENOSYS;
    return -1;
}

/* ── auth / master ────────────────────────────────────────────────────── */

int drmGetMagic(int fd, drm_magic_t *magic)
{
    if (check_fd(fd) < 0) return -1;
    if (magic) *magic = 1;
    return 0;
}

int drmAuthMagic(int fd, drm_magic_t magic)
{
    if (check_fd(fd) < 0) return -1;
    (void)magic;
    return 0;
}

int drmSetMaster(int fd)
{
    if (check_fd(fd) < 0) return -1;
    return 0;
}

int drmDropMaster(int fd)
{
    if (check_fd(fd) < 0) return -1;
    return 0;
}
