#include "drm_linux.h"
#include "drm.h"
#include "drm_ioctl.h"
#include "DisplaySurface.h"
#include "sdl_backend.h"

#include <IOSurface/IOSurface.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <math.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ── dynamic mode table (reads display resolution from plist) ────────── */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

static int get_display_count(void);
static void get_display_bounds(int idx, uint32_t *w, uint32_t *h, int *x, int *y);

/* Stable, single snapshot of the active display list. Index 0 is the main
 * display. This is the ONE source of truth for crtc<->display mapping, shared
 * by the DRM mode enumeration and the SDL window placement so the two never
 * disagree on which physical display a CRTC refers to. */
#define MAX_DISPLAYS 16
static CGDirectDisplayID g_displays[MAX_DISPLAYS];
static int g_display_count = 0;

static void ensure_displays(void) {
    if (g_display_count > 0) return;
    uint32_t count = 0;
    CGGetActiveDisplayList(0, NULL, &count);
    if (count == 0) {
        g_displays[0] = CGMainDisplayID();
        g_display_count = 1;
        return;
    }
    if (count > MAX_DISPLAYS) count = MAX_DISPLAYS;
    CGGetActiveDisplayList(count, g_displays, &count);
    /* Put the main display first so crtc 1 == primary monitor. */
    CGDirectDisplayID main = CGMainDisplayID();
    if (g_displays[0] != main) {
        for (uint32_t i = 0; i < count; i++) {
            if (g_displays[i] == main) {
                CGDirectDisplayID tmp = g_displays[0];
                g_displays[0] = main;
                g_displays[i] = tmp;
                break;
            }
        }
    }
    g_display_count = (int)count;
}

/* Public (used by the SDL backend for window placement). */
int mac_display_count(void) { ensure_displays(); return g_display_count; }
void mac_display_bounds(int idx, int *x, int *y, int *w, int *h) {
    ensure_displays();
    if (idx < 0 || idx >= g_display_count) { *x=0;*y=0;*w=1920;*h=1080; return; }
    CGRect b = CGDisplayBounds(g_displays[idx]);
    *x = (int)b.origin.x; *y = (int)b.origin.y;
    *w = (int)b.size.width; *h = (int)b.size.height;
}

static drmModeModeInfo g_modes[8];
static int g_mode_count;

static uint32_t get_display_refresh_rate(void)
{
    uint32_t refresh = 60;
    CGDirectDisplayID main_display = CGMainDisplayID();
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(main_display);
    if (mode) {
        double r = CGDisplayModeGetRefreshRate(mode);
        if (r > 0.0) {
            refresh = (uint32_t)(r + 0.5);
        } else {
            /* ProMotion displays return 0, default to 120 */
            refresh = 120;
        }
        CGDisplayModeRelease(mode);
    }
    if (refresh < 60) refresh = 60;
    // Clamp ProMotion/low reports to 120 for smoother FPS
    if (refresh < 120) refresh = 120;
    return refresh;
}

static void init_modes(void)
{
    if (g_mode_count > 0) return;

    int display_count = get_display_count();
    if (display_count < 1) display_count = 1;
    uint32_t refresh_rate = get_display_refresh_rate();

    for (int d = 0; d < display_count && g_mode_count < (int)(sizeof(g_modes)/sizeof(g_modes[0])) - 1; d++) {
        uint32_t pw, ph;
        int dx, dy;
        get_display_bounds(d, &pw, &ph, &dx, &dy);
        drmModeModeInfo *m = &g_modes[g_mode_count++];
        memset(m, 0, sizeof(*m));
        m->clock       = (pw * ph * refresh_rate + 500) / 1000;
        m->hdisplay    = pw;  m->hsync_start = pw + 88;
        m->hsync_end   = pw + 88 + 44;  m->htotal = pw + 88 + 44 + 168;
        m->vdisplay    = ph;  m->vsync_start = ph + 4;
        m->vsync_end   = ph + 4 + 5;   m->vtotal = ph + 4 + 5 + 36;
        m->vrefresh    = refresh_rate;
        m->type        = 0;
        snprintf(m->name, sizeof(m->name), "%ux%u", pw, ph);
    }
    // 1920x1080 fallback if we have room
    if (g_mode_count < (int)(sizeof(g_modes)/sizeof(g_modes[0]))) {
        drmModeModeInfo *m = &g_modes[g_mode_count++];
        memset(m, 0, sizeof(*m));
        m->clock       = (1920 * 1080 * refresh_rate + 500) / 1000;
        m->hdisplay    = 1920; m->hsync_start = 2008;
        m->hsync_end   = 2052; m->htotal = 2200;
        m->vdisplay    = 1080; m->vsync_start = 1084;
        m->vsync_end   = 1089; m->vtotal = 1125;
        m->vrefresh    = refresh_rate;
        m->type        = 0;
        snprintf(m->name, sizeof(m->name), "1920x1080");
    }
}
#define G_MODE_COUNT  g_mode_count
#define MAX_CRTCS 8

/* active CRTC state per CRTC (multi-monitor) */
static struct {
    uint32_t        crtc_fb_id;
    drmModeModeInfo crtc_mode;
    int             crtc_mode_valid;
    int             x, y;
} g_state[MAX_CRTCS];

static int get_display_count(void) {
    ensure_displays();
    int n = g_display_count;
    if (n < 1) n = 1;
    if (n > MAX_CRTCS) n = MAX_CRTCS;
    return n;
}

static void get_display_bounds(int idx, uint32_t *w, uint32_t *h, int *x, int *y) {
    ensure_displays();
    if (idx < 0 || idx >= g_display_count) {
        *w = 1920; *h = 1080; *x = 0; *y = 0;
        return;
    }
    CGRect b = CGDisplayBounds(g_displays[idx]);
    /* Report PIXEL resolution (points * scale) so the framebuffer Weston
     * allocates matches the SDL window's high-density drawable 1:1. */
    *w = (uint32_t)CGDisplayPixelsWide(g_displays[idx]);
    *h = (uint32_t)CGDisplayPixelsHigh(g_displays[idx]);
    *x = (int)b.origin.x;
    *y = (int)b.origin.y;
}

/* Virtual planes: one primary plane per CRTC so every output can enable.
 * Plane id N maps 1:1 to CRTC id N (see atomic FB_ID handling below). */
static uint32_t *g_plane_ids = NULL;
static int g_plane_count = 0;

static void ensure_planes(void) {
    if (g_plane_ids) return;
    int n = get_display_count();
    if (n < 1) n = 1;
    if (n > MAX_CRTCS) n = MAX_CRTCS;
    g_plane_ids = malloc(n * sizeof(uint32_t));
    for (int i = 0; i < n; i++) g_plane_ids[i] = (uint32_t)(i + 1);
    g_plane_count = n;
}

/* DRM event pipe — written by drmModePageFlip, read by drmHandleEvent.
 * wayland-mac.c dup2's a real pipe to fd DRM_VIRTUAL_FD at startup so
 * select/poll work natively on the virtual fd.  -1 = not initialised. */
int g_drm_event_pipe_write = -1;

/* Pending page-flip events. Each drmModePageFlip enqueues one; drmHandleEvent
 * dequeues and reports it with the correct CRTC id so Weston re-arms the right
 * output's repaint loop (critical for multi-monitor: a single global would only
 * ever re-arm CRTC 1, leaving every other output stuck on its first frame). */
typedef struct { uint32_t crtc_id; void *data; } flip_event_t;
#define MAX_FLIP_EVENTS 64
static flip_event_t g_flip_events[MAX_FLIP_EVENTS];
static int g_flip_ev_head = 0;
static int g_flip_ev_tail = 0;
static pthread_mutex_t g_flip_ev_lock = PTHREAD_MUTEX_INITIALIZER;

static void enqueue_flip_event(uint32_t crtc_id, void *data) {
    pthread_mutex_lock(&g_flip_ev_lock);
    int next = (g_flip_ev_head + 1) % MAX_FLIP_EVENTS;
    if (next == g_flip_ev_tail) {
        /* queue full: drop oldest */
        g_flip_ev_tail = (g_flip_ev_tail + 1) % MAX_FLIP_EVENTS;
    }
    g_flip_events[g_flip_ev_head].crtc_id = crtc_id;
    g_flip_events[g_flip_ev_head].data = data;
    g_flip_ev_head = next;
    pthread_mutex_unlock(&g_flip_ev_lock);
}

static int dequeue_flip_event(uint32_t *crtc_id, void **data) {
    pthread_mutex_lock(&g_flip_ev_lock);
    if (g_flip_ev_head == g_flip_ev_tail) {
        pthread_mutex_unlock(&g_flip_ev_lock);
        return 0;
    }
    *crtc_id = g_flip_events[g_flip_ev_tail].crtc_id;
    *data = g_flip_events[g_flip_ev_tail].data;
    g_flip_ev_tail = (g_flip_ev_tail + 1) % MAX_FLIP_EVENTS;
    pthread_mutex_unlock(&g_flip_ev_lock);
    return 1;
}

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
    case DRM_CAP_CRTC_IN_VBLANK_EVENT: *value = 1; break;
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

    int n = get_display_count();
    if (n < 1) n = 1;
    if (n > MAX_CRTCS) n = MAX_CRTCS;

    r->count_fbs        = 0;
    r->fbs              = NULL;

    r->count_crtcs      = n;
    r->crtcs            = malloc(n * sizeof(uint32_t));
    for (int i = 0; i < n; i++) r->crtcs[i] = i + 1;

    r->count_connectors = n;
    r->connectors       = malloc(n * sizeof(uint32_t));
    for (int i = 0; i < n; i++) r->connectors[i] = i + 1;

    r->count_encoders   = n;
    r->encoders         = malloc(n * sizeof(uint32_t));
    for (int i = 0; i < n; i++) r->encoders[i] = i + 1;

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
    int n = get_display_count();
    if (connector_id < 1 || connector_id > (uint32_t)n) { errno = ENOENT; return NULL; }

    drmModeConnector *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->connector_id      = connector_id;
    c->encoder_id        = connector_id;
    c->connector_type    = DRM_MODE_CONNECTOR_DisplayPort;
    c->connector_type_id = connector_id;
    c->connection        = DRM_MODE_CONNECTED;
    c->mmWidth           = 527;
    c->mmHeight          = 296;
    c->subpixel          = 0;

    init_modes();
    // For per-connector, return the mode matching that display's size as first mode
    // plus all modes as fallback
    int idx = (int)connector_id - 1;
    uint32_t pw, ph; int dx, dy;
    get_display_bounds(idx, &pw, &ph, &dx, &dy);
    // Find mode matching this display's size as first
    c->count_modes       = G_MODE_COUNT;
    c->modes             = malloc(G_MODE_COUNT * sizeof(drmModeModeInfo));
    // Put matching mode first if found
    int found = -1;
    for (int i = 0; i < G_MODE_COUNT; i++) {
        if (g_modes[i].hdisplay == pw && g_modes[i].vdisplay == ph) { found = i; break; }
    }
    if (found >= 0 && found != 0) {
        c->modes[0] = g_modes[found];
        int out = 1;
        for (int i = 0; i < G_MODE_COUNT; i++) if (i != found) c->modes[out++] = g_modes[i];
    } else {
        memcpy(c->modes, g_modes, G_MODE_COUNT * sizeof(drmModeModeInfo));
    }

    c->count_props       = 0;
    c->props             = NULL;
    c->prop_values       = NULL;

    c->count_encoders    = 1;
    c->encoders          = malloc(sizeof(uint32_t));
    c->encoders[0]       = connector_id;

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
    int n = get_display_count();
    if (encoder_id < 1 || encoder_id > (uint32_t)n) { errno = ENOENT; return NULL; }

    drmModeEncoder *e = calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->encoder_id     = encoder_id;
    e->encoder_type   = 10;
    e->crtc_id        = encoder_id;
    e->possible_crtcs = 1u << (encoder_id - 1);
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
    int n = get_display_count();
    if (crtc_id < 1 || crtc_id > (uint32_t)n) { errno = ENOENT; return NULL; }

    drmModeCrtc *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    int idx = (int)crtc_id - 1;
    c->crtc_id    = crtc_id;
    c->buffer_id  = g_state[idx].crtc_fb_id;
    c->x = g_state[idx].x;
    c->y = g_state[idx].y;
    c->width      = g_state[idx].crtc_mode_valid ? g_state[idx].crtc_mode.hdisplay : 0;
    c->height     = g_state[idx].crtc_mode_valid ? g_state[idx].crtc_mode.vdisplay : 0;
    c->mode_valid = g_state[idx].crtc_mode_valid;
    if (g_state[idx].crtc_mode_valid)
        c->mode = g_state[idx].crtc_mode;
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

    /* Create a display-compatible IOSurface via DisplaySurface_create().
     * This ensures the surface has the same pixel format, cache mode, and
     * properties as the CAWindowServer display pipeline, enabling
     * framebufferd to directly present it without any blitting. */
    DisplaySurfaceInfo dsi = DisplaySurface_create(width, height, kWSPixelFormatBGRA);
    IOSurfaceRef surf = dsi.surface;
    if (!surf) { errno = ENOMEM; return -1; }

    uint32_t p   = (uint32_t)IOSurfaceGetBytesPerRow(surf);
    size_t   sz  = (size_t)IOSurfaceGetAllocSize(surf);

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

/* ── GBM buffer handle table ────────────────────────────────────────────
 * GBM creates IOSurface-backed buffers with opaque handles.  When kmscube
 * passes those handles to drmModeAddFB, we need to find the IOSurface.  */

#define MAX_GBM_BUFS 64

typedef struct {
    uint32_t     handle;
    IOSurfaceRef surface;
} gbm_buf_entry_t;

static gbm_buf_entry_t g_gbm_bufs[MAX_GBM_BUFS];
static int             g_gbm_buf_count;

void drm_register_gbm_buffer(uint32_t handle, void *surface)
{
    if (!surface || handle == 0) return;
    for (int i = 0; i < g_gbm_buf_count; i++)
        if (g_gbm_bufs[i].handle == handle) {
            g_gbm_bufs[i].surface = (IOSurfaceRef)surface;
            return;
        }
    if (g_gbm_buf_count >= MAX_GBM_BUFS) return;
    g_gbm_bufs[g_gbm_buf_count].handle  = handle;
    g_gbm_bufs[g_gbm_buf_count].surface = (IOSurfaceRef)surface;
    g_gbm_buf_count++;
}

void drm_unregister_gbm_buffer(uint32_t handle)
{
    for (int i = 0; i < g_gbm_buf_count; i++)
        if (g_gbm_bufs[i].handle == handle) {
            g_gbm_bufs[i] = g_gbm_bufs[--g_gbm_buf_count];
            return;
        }
}

static IOSurfaceRef lookup_gbm_buffer(uint32_t handle)
{
    for (int i = 0; i < g_gbm_buf_count; i++)
        if (g_gbm_bufs[i].handle == handle)
            return g_gbm_bufs[i].surface;
    return NULL;
}

/* ── framebuffers ─────────────────────────────────────────────────────── */

int drmModeAddFB(int fd, uint32_t width, uint32_t height,
                 uint8_t depth, uint8_t bpp,
                 uint32_t pitch, uint32_t bo_handle,
                 uint32_t *buf_id)
{
    if (check_fd(fd) < 0) return -1;
    if (!buf_id) { errno = EINVAL; return -1; }

    /* Find the dumb buffer or GBM buffer backing this handle */
    IOSurfaceRef surf = NULL;
    for (int i = 0; i < MAX_DUMB_BUFS; i++) {
        if (g_dumb[i].handle == bo_handle) {
            surf = g_dumb[i].surface;
            break;
        }
    }
    if (!surf)
        surf = lookup_gbm_buffer(bo_handle);
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
    int n = get_display_count();
    if (crtc_id < 1 || crtc_id > (uint32_t)n) { errno = ENOENT; return -1; }
    (void)connectors; (void)count;

    int idx = (int)crtc_id - 1;
    g_state[idx].crtc_fb_id      = fb_id;
    g_state[idx].x = (int)x;
    g_state[idx].y = (int)y;
    g_state[idx].crtc_mode_valid = (mode != NULL);
    if (mode) g_state[idx].crtc_mode = *mode;

    // If a framebuffer is provided, present it immediately at the given position
    if (fb_id != 0) {
        IOSurfaceRef surf = fb_id_to_surface(fb_id);
        if (surf) {
            sdl_present_iosurface_for_crtc(surf, crtc_id, (int)x, (int)y);
        }
    }
    return 0;
}

int drmModePageFlip(int fd, uint32_t crtc_id, uint32_t fb_id,
                    uint32_t flags, void *user_data)
{
    if (check_fd(fd) < 0) return -1;
    int n = get_display_count();
    if (crtc_id < 1 || crtc_id > (uint32_t)n) { errno = ENOENT; return -1; }

    int idx = (int)crtc_id - 1;
    g_state[idx].crtc_fb_id = fb_id;

    IOSurfaceRef surf = fb_id_to_surface(fb_id);
    if (surf) {
        int x = g_state[idx].x;
        int y = g_state[idx].y;
        sdl_present_iosurface_for_crtc(surf, crtc_id, x, y);
    }

    int ret = 0;

    /* Signal page flip completion. Store the CRTC id so Weston re-arms the
     * correct output's repaint loop (essential for multi-monitor). */
    if (ret == 0 && g_drm_event_pipe_write >= 0) {
        enqueue_flip_event(crtc_id, user_data);
        char byte = 1;
        ssize_t w = write(g_drm_event_pipe_write, &byte, 1);
        (void)w;
    }

    return ret;
}

int drmHandleEvent(int fd, drmEventContextPtr evctx)
{
    if (fd != DRM_VIRTUAL_FD) { errno = EBADF; return -1; }

    /* Read one event byte from the pipe */
    char byte;
    ssize_t n = read(fd, &byte, 1);
    if (n <= 0) {
        if (n == 0) errno = EAGAIN;
        return -1;
    }

    if (byte == 1 && evctx) {
        /* Drain all queued flip events for this wakeup (a single commit may
         * have enqueued completions for multiple CRTCs). */
        for (;;) {
            uint32_t crtc_id = 1;
            void *data = NULL;
            if (!dequeue_flip_event(&crtc_id, &data)) break;
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            /* page_flip_handler2 (v2+) provides CRTC ID required for atomic mode.
             * The timestamp MUST be in CLOCK_MONOTONIC (Weston's presentation
             * clock); passing CLOCK_REALTIME makes Weston think the next
             * presentation is ~1.7e12 ms away and throttle repaints to ~0 fps. */
            if (evctx->version >= 2 && evctx->page_flip_handler2) {
                evctx->page_flip_handler2(fd, 0,
                                          (unsigned int)ts.tv_sec,
                                          (unsigned int)(ts.tv_nsec / 1000),
                                          crtc_id,
                                          data);
            } else if (evctx->page_flip_handler) {
                evctx->page_flip_handler(fd, 0,
                                          (unsigned int)ts.tv_sec,
                                          (unsigned int)(ts.tv_nsec / 1000),
                                          data);
            }
        }
    }
    return 1;
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

/* ═══════════════════════════════════════════════════════════════════════
 *  Property / Plane / Atomic / Cursor / Sync  implementation
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── client capabilities ─────────────────────────────────────────────── */

static struct {
    uint64_t universal_planes;
    uint64_t atomic;
} g_client_caps;

int drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
    if (check_fd(fd) < 0) return -1;
    switch (capability) {
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
        g_client_caps.universal_planes = value;
        return 0;
    case DRM_CLIENT_CAP_ATOMIC:
        g_client_caps.atomic = value;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

/* ── property store ──────────────────────────────────────────────────── */

#define MAX_PROPS         64
#define MAX_PROPS_PER_OBJ 16
#define MAX_BLOBS         32

typedef struct {
    uint32_t id;
    uint32_t flags;
    char     name[32];
    uint32_t count_values;
    uint64_t values[4];          
    uint32_t count_enums;
    struct {
        uint64_t value;
        char     name[32];
    } enums[8];
    uint32_t count_blobs;
} prop_def_t;

static prop_def_t  g_props[MAX_PROPS];
static int         g_prop_count;

typedef struct {
    uint32_t obj_id;
    uint32_t obj_type;
    int      prop_count;
    uint32_t prop_ids[MAX_PROPS_PER_OBJ];
    uint64_t prop_vals[MAX_PROPS_PER_OBJ];
} obj_props_t;

static obj_props_t g_obj_props[16];
static int         g_obj_prop_count;

/* IN_FORMATS blob: reports supported formats + LINEAR modifier */
typedef struct { uint32_t format; uint64_t modifier; } fmt_mod_pair;

static fmt_mod_pair g_in_formats[] = {
    { DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR },
    { DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR },
    { DRM_FORMAT_XBGR8888, DRM_FORMAT_MOD_LINEAR },
    { DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR },
};
#define G_IN_FORMATS_COUNT  ((int)(sizeof(g_in_formats)/sizeof(g_in_formats[0])))

static uint32_t g_blob_ids[MAX_BLOBS];
static uint32_t g_blob_sizes[MAX_BLOBS];
static void    *g_blob_data[MAX_BLOBS];
static int      g_blob_count;
static uint32_t g_next_blob_id = 256;

static int add_prop(const char *name, uint32_t flags,
                    uint32_t nvals, const uint64_t *vals)
{
    if (g_prop_count >= MAX_PROPS) return -1;
    int i = g_prop_count++;
    g_props[i].id = i + 1;
    g_props[i].flags = flags;
    strncpy(g_props[i].name, name, sizeof(g_props[i].name) - 1);
    g_props[i].count_values = nvals;
    for (uint32_t j = 0; j < nvals && j < 4; j++)
        g_props[i].values[j] = vals[j];
    g_props[i].count_enums = 0;
    return g_props[i].id;
}

static int add_enum_prop(const char *name, uint32_t flags,
                         const char *const *enum_names,
                         const uint64_t *enum_vals, int enum_count)
{
    int id = add_prop(name, flags, 0, NULL);
    if (id < 0) return -1;
    int i = id - 1;
    g_props[i].count_enums = enum_count;
    for (int j = 0; j < enum_count && j < 8; j++) {
        g_props[i].enums[j].value = enum_vals ? enum_vals[j] : j;
        strncpy(g_props[i].enums[j].name,
                enum_names[j], sizeof(g_props[i].enums[j].name) - 1);
    }
    return id;
}

static int add_blob_prop(const char *name, uint32_t flags)
{
    return add_prop(name, flags | DRM_MODE_PROP_BLOB, 0, NULL);
}

static void ensure_properties(void)
{
    if (g_prop_count > 0) return;

    /* Connector properties */
    {
        const char *enames[] = {"OK", "off"};
        uint64_t evals[] = {0, 1};
        add_enum_prop("DPMS",
            DRM_MODE_PROP_ENUM | DRM_MODE_PROP_ATOMIC, enames, evals, 2);
    }
    {
        uint64_t rng[] = {0, 1};
        add_prop("CRTC_ID",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_IMMUTABLE,
            2, rng);
    }
    add_blob_prop("EDID", DRM_MODE_PROP_IMMUTABLE);

    /* CRTC properties */
    {
        uint64_t rng[] = {0, 1};
        add_prop("ACTIVE",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    add_blob_prop("MODE_ID", DRM_MODE_PROP_ATOMIC);

    /* Plane properties */
    add_blob_prop("IN_FORMATS",
        DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_BLOB);
    {
        uint64_t rng[] = {0, 0xFFFFFFFF};
        add_prop("FB_ID",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    {
        uint64_t rng[] = {0, 0xFFFFFFFF};
        add_prop("CRTC_X",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    {
        uint64_t rng[] = {0, 0xFFFFFFFF};
        add_prop("CRTC_Y",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    {
        uint64_t rng[] = {0, 0xFFFFFFFF};
        add_prop("CRTC_W",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    {
        uint64_t rng[] = {0, 0xFFFFFFFF};
        add_prop("CRTC_H",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    {
        uint64_t rng[] = {0, 0xFFFFFFFF};
        add_prop("SRC_X",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    {
        uint64_t rng[] = {0, 0xFFFFFFFF};
        add_prop("SRC_Y",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    {
        uint64_t rng[] = {0, 0xFFFFFFFF};
        add_prop("SRC_W",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    {
        uint64_t rng[] = {0, 0xFFFFFFFF};
        add_prop("SRC_H",
            DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC, 2, rng);
    }
    {
        const char *tnames[] = {"Overlay", "Primary", "Cursor"};
        uint64_t tvals[] = {
            DRM_PLANE_TYPE_OVERLAY,
            DRM_PLANE_TYPE_PRIMARY,
            DRM_PLANE_TYPE_CURSOR
        };
        add_enum_prop("type",
            DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_ENUM,
            tnames, tvals, 3);
    }
}

static uint32_t lookup_prop_id(const char *name)
{
    for (int i = 0; i < g_prop_count; i++)
        if (strcmp(g_props[i].name, name) == 0)
            return g_props[i].id;
    return 0;
}

/* Cached property IDs — looked up once, reused in hot paths */
static struct {
    uint32_t fb_id;
    uint32_t crtc_x;
    uint32_t crtc_y;
    uint32_t crtc_w;
    uint32_t crtc_h;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_w;
    uint32_t src_h;
    uint32_t crtc_id;
    uint32_t active;
    uint32_t mode_id;
    uint32_t dpms;
    uint32_t type;
    uint32_t in_formats;
} g_cached_prop_ids;

static void ensure_cached_prop_ids(void)
{
    if (g_cached_prop_ids.fb_id) return; /* already cached */
    ensure_properties();
    g_cached_prop_ids.fb_id      = lookup_prop_id("FB_ID");
    g_cached_prop_ids.crtc_x     = lookup_prop_id("CRTC_X");
    g_cached_prop_ids.crtc_y     = lookup_prop_id("CRTC_Y");
    g_cached_prop_ids.crtc_w     = lookup_prop_id("CRTC_W");
    g_cached_prop_ids.crtc_h     = lookup_prop_id("CRTC_H");
    g_cached_prop_ids.src_x      = lookup_prop_id("SRC_X");
    g_cached_prop_ids.src_y      = lookup_prop_id("SRC_Y");
    g_cached_prop_ids.src_w      = lookup_prop_id("SRC_W");
    g_cached_prop_ids.src_h      = lookup_prop_id("SRC_H");
    g_cached_prop_ids.crtc_id    = lookup_prop_id("CRTC_ID");
    g_cached_prop_ids.active     = lookup_prop_id("ACTIVE");
    g_cached_prop_ids.mode_id    = lookup_prop_id("MODE_ID");
    g_cached_prop_ids.dpms       = lookup_prop_id("DPMS");
    g_cached_prop_ids.type       = lookup_prop_id("type");
    g_cached_prop_ids.in_formats = lookup_prop_id("IN_FORMATS");
}

/* Find or create object property record */
static obj_props_t *get_obj_props(uint32_t obj_id, uint32_t obj_type)
{
    for (int i = 0; i < g_obj_prop_count; i++)
        if (g_obj_props[i].obj_id == obj_id
            && g_obj_props[i].obj_type == obj_type)
            return &g_obj_props[i];
    if (g_obj_prop_count >= (int)(sizeof(g_obj_props)/sizeof(g_obj_props[0])))
        return NULL;
    int i = g_obj_prop_count++;
    g_obj_props[i].obj_id = obj_id;
    g_obj_props[i].obj_type = obj_type;
    g_obj_props[i].prop_count = 0;
    return &g_obj_props[i];
}

static void set_obj_prop(obj_props_t *op, uint32_t prop_id, uint64_t val)
{
    for (int i = 0; i < op->prop_count; i++)
        if (op->prop_ids[i] == prop_id) {
            op->prop_vals[i] = val;
            return;
        }
    if (op->prop_count >= MAX_PROPS_PER_OBJ) return;
    int i = op->prop_count++;
    op->prop_ids[i] = prop_id;
    op->prop_vals[i] = val;
}

static uint64_t get_obj_prop(obj_props_t *op, uint32_t prop_id)
{
    for (int i = 0; i < op->prop_count; i++)
        if (op->prop_ids[i] == prop_id)
            return op->prop_vals[i];
    return 0;
}

/* ensure each object has the right default properties */
static void ensure_obj_properties(void)
{
    ensure_cached_prop_ids();

    /* CRTCs 1..N: default ACTIVE=1, MODE_ID=0 */
    int n = get_display_count();
    if (n < 1) n = 1;
    if (n > MAX_CRTCS) n = MAX_CRTCS;
    for (int i = 0; i < n; i++) {
        obj_props_t *cr = get_obj_props((uint32_t)(i + 1), DRM_MODE_OBJECT_CRTC);
        if (cr->prop_count == 0) {
            set_obj_prop(cr, g_cached_prop_ids.active, 1);
            set_obj_prop(cr, g_cached_prop_ids.mode_id, 0);
        }
        obj_props_t *co = get_obj_props((uint32_t)(i + 1), DRM_MODE_OBJECT_CONNECTOR);
        if (co->prop_count == 0) {
            set_obj_prop(co, g_cached_prop_ids.dpms, 0);
            set_obj_prop(co, g_cached_prop_ids.crtc_id, 0);
        }
    }

    /* One primary plane per CRTC (1:1). All are PRIMARY type. */
    ensure_planes();
    for (int i = 0; i < g_plane_count; i++) {
        uint32_t pid = (uint32_t)(i + 1);
        obj_props_t *pp = get_obj_props(pid, DRM_MODE_OBJECT_PLANE);
        if (pp->prop_count == 0) {
            set_obj_prop(pp, g_cached_prop_ids.type, DRM_PLANE_TYPE_PRIMARY);
            set_obj_prop(pp, g_cached_prop_ids.fb_id, 0);
            set_obj_prop(pp, g_cached_prop_ids.crtc_id, pid);
            set_obj_prop(pp, g_cached_prop_ids.crtc_x, 0);
            set_obj_prop(pp, g_cached_prop_ids.crtc_y, 0);
            set_obj_prop(pp, g_cached_prop_ids.crtc_w, 0);
            set_obj_prop(pp, g_cached_prop_ids.crtc_h, 0);
            set_obj_prop(pp, g_cached_prop_ids.src_x, 0);
            set_obj_prop(pp, g_cached_prop_ids.src_y, 0);
            set_obj_prop(pp, g_cached_prop_ids.src_w, 0);
            set_obj_prop(pp, g_cached_prop_ids.src_h, 0);
        }
    }
}

/* ── blob management ─────────────────────────────────────────────────── */

static uint32_t create_in_formats_blob(void)
{
    size_t sz   = sizeof(g_in_formats);
    void  *data = malloc(sz);
    if (!data) return 0;
    memcpy(data, g_in_formats, sz);

    int slot = -1;
    for (int i = 0; i < MAX_BLOBS; i++)
        if (g_blob_data[i] == NULL) { slot = i; break; }
    if (slot < 0) { free(data); return 0; }

    uint32_t id = g_next_blob_id++;
    g_blob_ids[slot]  = id;
    g_blob_sizes[slot]= sz;
    g_blob_data[slot] = data;
    return id;
}

static uint32_t g_in_formats_blob_id;

static uint32_t get_or_create_in_formats_blob(void)
{
    if (g_in_formats_blob_id) return g_in_formats_blob_id;
    g_in_formats_blob_id = create_in_formats_blob();
    return g_in_formats_blob_id;
}

/* ── property API ────────────────────────────────────────────────────── */

drmModePropertyResPtr drmModeGetProperty(int fd, uint32_t property_id)
{
    if (check_fd(fd) < 0) return NULL;
    ensure_properties();
    if (property_id < 1 || property_id > (uint32_t)g_prop_count) {
        errno = ENOENT;
        return NULL;
    }
    const prop_def_t *pd = &g_props[property_id - 1];
    drmModePropertyRes *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->prop_id      = pd->id;
    p->flags        = pd->flags;
    memcpy(p->name, pd->name, sizeof(p->name));

    p->count_values = pd->count_values;
    if (pd->count_values > 0) {
        p->values = malloc(pd->count_values * sizeof(uint64_t));
        if (p->values) memcpy(p->values, pd->values, pd->count_values * sizeof(uint64_t));
    }

    p->count_enums  = pd->count_enums;
    if (pd->count_enums > 0) {
        size_t esz = sizeof(p->enums[0]) * pd->count_enums;
        p->enums = malloc(esz);
        if (p->enums) memcpy(p->enums, pd->enums, esz);
    }

    /* For IN_FORMATS, return the current blob id */
    if (strcmp(pd->name, "IN_FORMATS") == 0 && pd->count_blobs == 0) {
        p->count_blobs = 1;
        p->blob_ids = malloc(sizeof(uint32_t));
        if (p->blob_ids) p->blob_ids[0] = get_or_create_in_formats_blob();
    }

    return p;
}

void drmModeFreeProperty(drmModePropertyResPtr ptr)
{
    if (!ptr) return;
    free(ptr->values);
    free(ptr->enums);
    free(ptr->blob_ids);
    free(ptr);
}

drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd,
    uint32_t object_id, uint32_t object_type)
{
    if (check_fd(fd) < 0) return NULL;
    ensure_obj_properties();

    obj_props_t *op = get_obj_props(object_id, object_type);
    if (!op) { errno = ENOENT; return NULL; }

    drmModeObjectProperties *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->count_props = op->prop_count;
    p->props       = malloc(p->count_props * sizeof(uint32_t));
    p->prop_values = malloc(p->count_props * sizeof(uint64_t));
    if (p->props && p->prop_values) {
        memcpy(p->props, op->prop_ids, p->count_props * sizeof(uint32_t));
        memcpy(p->prop_values, op->prop_vals, p->count_props * sizeof(uint64_t));
    }
    return p;
}

void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr ptr)
{
    if (!ptr) return;
    free(ptr->props);
    free(ptr->prop_values);
    free(ptr);
}

drmModePropertyBlobResPtr drmModeGetPropertyBlob(int fd, uint32_t blob_id)
{
    if (check_fd(fd) < 0) return NULL;
    for (int i = 0; i < MAX_BLOBS; i++)
        if (g_blob_data[i] != NULL && g_blob_ids[i] == blob_id) {
            drmModePropertyBlobRes *b = calloc(1, sizeof(*b));
            if (!b) break;
            b->id     = blob_id;
            b->length = g_blob_sizes[i];
            b->data   = malloc(b->length);
            if (b->data) memcpy(b->data, g_blob_data[i], b->length);
            return b;
        }
    errno = ENOENT;
    return NULL;
}

void drmModeFreePropertyBlob(drmModePropertyBlobResPtr ptr)
{
    if (!ptr) return;
    free(ptr->data);
    free(ptr);
}

int drmModeCreatePropertyBlob(int fd, const void *data, size_t length,
                              uint32_t *blob_id)
{
    if (check_fd(fd) < 0) return -1;
    if (!data || !blob_id) { errno = EINVAL; return -1; }

    int slot = -1;
    for (int i = 0; i < MAX_BLOBS; i++)
        if (g_blob_data[i] == NULL) { slot = i; break; }
    if (slot < 0) { errno = ENOMEM; return -1; }

    void *copy = malloc(length);
    if (!copy) { errno = ENOMEM; return -1; }
    memcpy(copy, data, length);

    uint32_t id = g_next_blob_id++;
    g_blob_ids[slot]   = id;
    g_blob_sizes[slot] = length;
    g_blob_data[slot]  = copy;
    *blob_id = id;
    return 0;
}

int drmModeDestroyPropertyBlob(int fd, uint32_t blob_id)
{
    if (check_fd(fd) < 0) return -1;
    for (int i = 0; i < MAX_BLOBS; i++)
        if (g_blob_data[i] != NULL && g_blob_ids[i] == blob_id) {
            free(g_blob_data[i]);
            g_blob_data[i] = NULL;
            return 0;
        }
    errno = ENOENT;
    return -1;
}

/* ── plane resources ─────────────────────────────────────────────────── */

/* default supported formats per plane */
static uint32_t g_primary_formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
static uint32_t g_cursor_formats[]  = { DRM_FORMAT_ARGB8888 };
static uint32_t g_overlay_formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };

drmModePlaneResPtr drmModeGetPlaneResources(int fd)
{
    if (check_fd(fd) < 0) return NULL;
    if (!g_client_caps.universal_planes) {
        errno = EOPNOTSUPP;
        return NULL;
    }

    ensure_planes();

    drmModePlaneRes *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->count_planes = g_plane_count;
    r->planes = malloc(g_plane_count * sizeof(uint32_t));
    if (r->planes) memcpy(r->planes, g_plane_ids,
                          g_plane_count * sizeof(uint32_t));
    return r;
}

void drmModeFreePlaneResources(drmModePlaneResPtr ptr)
{
    if (!ptr) return;
    free(ptr->planes);
    free(ptr);
}

drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id)
{
    if (check_fd(fd) < 0) return NULL;
    ensure_obj_properties();
    ensure_cached_prop_ids();
    ensure_planes();

    drmModePlane *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->plane_id = plane_id;
    /* Plane N is bound to CRTC N only, keeping the 1:1 present mapping. */
    p->possible_crtcs = (plane_id >= 1 && plane_id <= (uint32_t)g_plane_count)
                            ? (1u << (plane_id - 1))
                            : 1u;
    p->gamma_size = 256;
    p->crtc_id    = plane_id;
    p->fb_id      = (uint32_t)get_obj_prop(
                        get_obj_props(plane_id, DRM_MODE_OBJECT_PLANE),
                        g_cached_prop_ids.fb_id);

    /* Every virtual plane is a primary plane (1:1 with a CRTC). Cursor uses
     * the Weston software fallback, so we don't expose separate cursor planes. */
    if (plane_id >= 1 && plane_id <= (uint32_t)g_plane_count) {
        p->count_formats = 2;
        p->formats = malloc(2 * sizeof(uint32_t));
        if (p->formats) { p->formats[0] = DRM_FORMAT_XRGB8888;
                           p->formats[1] = DRM_FORMAT_ARGB8888; }
        p->format_modifiers = NULL;
    } else {
        free(p->formats);
        free(p);
        errno = ENOENT;
        return NULL;
    }
    return p;
}

void drmModeFreePlane(drmModePlanePtr ptr)
{
    if (!ptr) return;
    free(ptr->formats);
    free(ptr->format_modifiers);
    free(ptr);
}

/* ── atomic ──────────────────────────────────────────────────────────── */

#define MAX_ATOMIC_PROPS 64

struct _drmModeAtomicReq {
    int      prop_count;
    uint32_t obj_ids[MAX_ATOMIC_PROPS];
    uint32_t prop_ids[MAX_ATOMIC_PROPS];
    uint64_t values[MAX_ATOMIC_PROPS];
};

drmModeAtomicReq *drmModeAtomicAlloc(void)
{
    drmModeAtomicReq *req = calloc(1, sizeof(*req));
    return req;
}

void drmModeAtomicFree(drmModeAtomicReq *req)
{
    free(req);
}

int drmModeAtomicAddProperty(drmModeAtomicReq *req,
    uint32_t object_id, uint32_t property_id, uint64_t value)
{
    if (!req) { errno = EINVAL; return -1; }
    if (req->prop_count >= MAX_ATOMIC_PROPS) { errno = ENOMEM; return -1; }
    int i = req->prop_count++;
    req->obj_ids[i]    = object_id;
    req->prop_ids[i]   = property_id;
    req->values[i]     = value;
    return req->prop_count; /* real libdrm returns count on success */
}

int drmModeAtomicCommit(int fd, drmModeAtomicReq *req,
                        uint32_t flags, void *user_data)
{
    if (check_fd(fd) < 0) return -1;
    if (!req) { errno = EINVAL; return -1; }

    ensure_obj_properties();

    /* Find cursor plane props for tracking position changes */
    obj_props_t *cursor_props = NULL;
    for (int j = 0; j < g_obj_prop_count; j++)
        if (g_obj_props[j].obj_id == 2) { cursor_props = &g_obj_props[j]; break; }

    /* Apply all property changes */
    uint32_t new_fb_id = 0;
    uint32_t new_plane_id = 0;
    /* A single atomic commit can flip multiple CRTCs at once (e.g. the
     * initial combined modeset).  Track every CRTC that actually flips so
     * each output's repaint loop is re-armed (essential for multi-monitor). */
    uint32_t flip_crtcs[8];
    int flip_n = 0;

    for (int i = 0; i < req->prop_count; i++) {
        uint32_t obj_id  = req->obj_ids[i];
        uint32_t prop_id = req->prop_ids[i];
        uint64_t val     = req->values[i];

        /* Find the object's prop record (matched by obj_id only) */
        obj_props_t *op = NULL;
        for (int j = 0; j < g_obj_prop_count; j++)
            if (g_obj_props[j].obj_id == obj_id) {
                op = &g_obj_props[j]; break;
            }
        if (!op) continue;

        set_obj_prop(op, prop_id, val);

        /* Track/send FB_ID changes - multi-monitor: map plane to CRTC */
        if (prop_id == g_cached_prop_ids.fb_id) {
            // Every virtual plane is a primary plane (no cursor planes are
            // exposed), so never skip. Primary plane N maps to CRTC N.
            // Determine CRTC for this plane: primary planes 1..N map to CRTC 1..N
            uint32_t crtc_id = obj_id;
            // For overlay planes, try to find crtc_id via plane's CRTC_ID property
            if (crtc_id > (uint32_t)get_display_count()) {
                // Fallback: try to get CRTC_ID from plane's obj_props
                obj_props_t *plane_props = op;
                uint64_t crtc_val = 0;
                for (int k = 0; k < plane_props->prop_count; k++) {
                    if (plane_props->prop_ids[k] == g_cached_prop_ids.crtc_id) {
                        crtc_val = plane_props->prop_vals[k];
                        break;
                    }
                }
                if (crtc_val > 0) crtc_id = (uint32_t)crtc_val;
                else crtc_id = 1;
            }
            int crtc_idx = (int)crtc_id - 1;
            if (crtc_idx < 0 || crtc_idx >= MAX_CRTCS) crtc_idx = 0;
            IOSurfaceRef surf = fb_id_to_surface((uint32_t)val);
            if (surf) {
                // Get plane's CRTC_X/Y for positioning (multi-monitor offset)
                int x = g_state[crtc_idx].x;
                int y = g_state[crtc_idx].y;
                // Try to get from plane's props if set in this atomic batch
                for (int k = 0; k < op->prop_count; k++) {
                    if (op->prop_ids[k] == g_cached_prop_ids.crtc_x) x = (int)op->prop_vals[k];
                    if (op->prop_ids[k] == g_cached_prop_ids.crtc_y) y = (int)op->prop_vals[k];
                }
                sdl_present_iosurface_for_crtc(surf, crtc_id, x, y);
                g_state[crtc_idx].crtc_fb_id = (uint32_t)val;
            }
            new_fb_id = (uint32_t)val;
            new_plane_id = obj_id;
            if (flip_n < 8) {
                int seen = 0;
                for (int f = 0; f < flip_n; f++)
                    if (flip_crtcs[f] == crtc_id) { seen = 1; break; }
                if (!seen) flip_crtcs[flip_n++] = crtc_id;
            }
        }
        // Track CRTC_X/Y changes for planes to update g_state position
        if (prop_id == g_cached_prop_ids.crtc_x || prop_id == g_cached_prop_ids.crtc_y) {
            // Find which CRTC this plane belongs to
            uint32_t crtc_id = obj_id;
            if (crtc_id > (uint32_t)get_display_count()) continue;
            int idx = (int)crtc_id - 1;
            if (prop_id == g_cached_prop_ids.crtc_x) g_state[idx].x = (int)val;
            if (prop_id == g_cached_prop_ids.crtc_y) g_state[idx].y = (int)val;
        }
    }

    /* Send page flip event for primary plane if detected */
    if (new_fb_id > 0) {
        // Update g_state for the plane that had FB change (use new_plane_id mapping)
        int idx = (int)new_plane_id - 1;
        if (idx >= 0 && idx < MAX_CRTCS) {
            g_state[idx].crtc_fb_id = new_fb_id;
        } else {
            g_state[0].crtc_fb_id = new_fb_id;
        }
    }

    /* Cursor plane position changes are ignored in SDL mode (software cursor) */

    /* Signal page flip completion when PAGE_FLIP_EVENT is requested.
     * Use the CRTC id captured from the FB_ID change so the correct output's
     * repaint loop is re-armed (multi-monitor). */
    if (flags & DRM_MODE_PAGE_FLIP_EVENT) {
        if (flip_n == 0) flip_crtcs[flip_n++] = 1;
        for (int f = 0; f < flip_n; f++) {
            enqueue_flip_event(flip_crtcs[f], user_data);
        }
        if (g_drm_event_pipe_write >= 0) {
            char byte = 1;
            ssize_t w = write(g_drm_event_pipe_write, &byte, 1);
            (void)w;
        }
    }

    return 0;
}

/* ── cursor ──────────────────────────────────────────────────────────── */

static struct {
    uint32_t    crtc_id;
    uint32_t    bo_handle;
    uint32_t    width;
    uint32_t    height;
    int         x;
    int         y;
    bool        active;
} g_cursor;

int drmModeSetCursor(int fd, uint32_t crtc_id, uint32_t bo_handle,
                     uint32_t width, uint32_t height)
{
    (void)fd; (void)crtc_id; (void)bo_handle; (void)width; (void)height;
    errno = ENOTSUP;
    return -1;
}

int drmModeMoveCursor(int fd, uint32_t crtc_id, int x, int y)
{
    (void)fd; (void)crtc_id; (void)x; (void)y;
    errno = ENOTSUP;
    return -1;
}

/* ── sync objects ────────────────────────────────────────────────────── */

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
    if (check_fd(fd) < 0) return -1;
    (void)flags;
    if (handle) *handle = 42;  /* dummy handle */
    return 0;
}

int drmSyncobjDestroy(int fd, uint32_t handle)
{
    if (check_fd(fd) < 0) return -1;
    (void)handle;
    return 0;
}

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
    if (check_fd(fd) < 0) return -1;
    (void)handle; (void)sync_file_fd;
    return 0;
}

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
    if (check_fd(fd) < 0) return -1;
    (void)handle;
    if (sync_file_fd) *sync_file_fd = -1;
    errno = ENOSYS;
    return -1;
}

int drmSyncobjFDToHandle(int fd, int sync_file_fd, uint32_t *handle)
{
    if (check_fd(fd) < 0) return -1;
    (void)sync_file_fd;
    if (handle) *handle = 42;
    return 0;
}

int drmSyncobjHandleToFD(int fd, uint32_t handle, int *sync_file_fd)
{
    if (check_fd(fd) < 0) return -1;
    (void)handle;
    if (sync_file_fd) *sync_file_fd = -1;
    errno = ENOSYS;
    return -1;
}

/* ── prime (properly stubbed) ────────────────────────────────────────── */

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
    if (check_fd(fd) < 0) return -1;
    (void)handle; (void)flags;
    /* Create a dummy fd so compositors don't crash on dma-buf import */
    if (prime_fd) {
        int p[2];
        if (pipe(p) < 0) { errno = ENOSYS; return -1; }
        close(p[1]);  /* close write end — compositor will get EOF on read */
        *prime_fd = p[0];
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
    if (check_fd(fd) < 0) return -1;
    (void)prime_fd;
    if (handle) *handle = 1;  /* dummy handle */
    close(prime_fd);
    return 0;
}

/* ── fb2 with modifiers ──────────────────────────────────────────────── */

int drmModeAddFB2WithModifiers(int fd, uint32_t width, uint32_t height,
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

const char *
drmGetFormatModifierName(uint64_t modifier)
{
    if (modifier == DRM_FORMAT_MOD_LINEAR)
        return strdup("Linear");
    return NULL;
}

int
drmCloseBufferHandle(int fd, int handle)
{
    if (check_fd(fd) < 0) return -1;
    (void)handle;
    return 0;
}

int
drmModeConnectorSetProperty(int fd, uint32_t connector_id,
                            uint32_t property_id, uint64_t value)
{
    if (check_fd(fd) < 0) return -1;
    (void)connector_id;(void)property_id;(void)value;
    return 0;
}

int
drmModeCrtcSetGamma(int fd, uint32_t crtc_id, uint32_t size,
                    uint16_t *red, uint16_t *green, uint16_t *blue)
{
    if (check_fd(fd) < 0) return -1;
    (void)crtc_id;(void)size;(void)red;(void)green;(void)blue;
    return 0;
}

int
drmModeSetPlane(int fd, uint32_t plane_id, uint32_t crtc_id,
                uint32_t fb_id, uint32_t flags,
                int32_t crtc_x, int32_t crtc_y,
                uint32_t crtc_w, uint32_t crtc_h,
                uint32_t src_x, uint32_t src_y,
                uint32_t src_w, uint32_t src_h)
{
    if (check_fd(fd) < 0) return -1;
    (void)plane_id;(void)crtc_id;(void)fb_id;(void)flags;
    (void)crtc_x;(void)crtc_y;(void)crtc_w;(void)crtc_h;
    (void)src_x;(void)src_y;(void)src_w;(void)src_h;
    return 0;
}

int
drmWaitVBlank(int fd, drmVBlank *vbl)
{
    if (check_fd(fd) < 0) return -1;
    (void)vbl;
    return 0;
}

bool
drmModeFormatModifierBlobIterNext(const drmModePropertyBlobRes *blob,
                                  drmModeFormatModifierIterator *iter)
{
    (void)blob;
    (void)iter;
    return false;
}

const char *
drmGetFormatModifierVendor(uint64_t modifier)
{
    if (modifier == DRM_FORMAT_MOD_LINEAR)
        return strdup("NONE");
    return NULL;
}
