#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#include <SDL3/SDL.h>
#include <CoreGraphics/CoreGraphics.h>
#include <QuartzCore/QuartzCore.h>
#include <pthread.h>

/* Private CGS API: lets us hide the cursor even when this process is not the
 * foreground application (our desktop windows can sit behind other apps).
 * Matches the technique used by Synergy / cursor-hiding tools. */
extern int _CGSDefaultConnection(void);
extern CGError CGSSetConnectionProperty(int cid, int targetCID, CFStringRef key, CFTypeRef value);

// ── SkyLight private API for desktop window (multi-monitor) ───────────
extern int SLSMainConnectionID(void);
extern uint64_t SLSSpaceCreate(int cid, int one, int zero);
extern CGError SLSSpaceSetAbsoluteLevel(int cid, uint64_t sid, int level);
extern CGError SLSShowSpaces(int cid, CFArrayRef space_list);
extern CGError SLSSpaceAddWindowsAndRemoveFromSpaces(int cid, uint64_t sid, CFArrayRef window_list, int flags);
extern CFArrayRef SLSCopyManagedDisplays(int cid);
extern uint64_t SLSManagedDisplayGetCurrentSpace(int cid, CFStringRef uuid);
extern CGError SLSAddWindowsToSpaces(int cid, CFArrayRef window_list, CFArrayRef space_list);
static uint64_t g_desktopSpace = 0;

/* ── macOS system cursor hide/show ──────────────────────────────────────
 * Weston draws its own software cursor, so once the desktop windows take
 * over the displays we hide the real macOS cursor.  It is restored on a
 * clean exit and via a signal handler (Ctrl-C / terminate). */
static bool g_cursor_hidden = false;

// hid_to_evdev mapping (must be before raw_mouse_tap_cb)
static const uint16_t hid_to_evdev[256] = {
    [0x04] = 30, [0x05] = 48, [0x06] = 46, [0x07] = 32,
    [0x08] = 18, [0x09] = 33, [0x0A] = 34, [0x0B] = 35,
    [0x0C] = 23, [0x0D] = 36, [0x0E] = 37, [0x0F] = 38,
    [0x10] = 50, [0x11] = 49, [0x12] = 24, [0x13] = 19,
    [0x14] = 16, [0x15] = 19, [0x16] = 31, [0x17] = 20,
    [0x18] = 22, [0x19] = 47, [0x1A] = 17, [0x1B] = 45,
    [0x1C] = 21, [0x1D] = 44, [0x1E] = 2,  [0x1F] = 3,
    [0x20] = 4,  [0x21] = 5,  [0x22] = 6,  [0x23] = 7,
    [0x24] = 8,  [0x25] = 9,  [0x26] = 10, [0x27] = 11,
    [0x28] = 28, [0x29] = 1,  [0x2A] = 14, [0x2B] = 15,
    [0x2C] = 57, [0x2D] = 12, [0x2E] = 13, [0x2F] = 26,
    [0x30] = 27, [0x31] = 43, [0x33] = 39, [0x34] = 40,
    [0x35] = 41, [0x36] = 51, [0x37] = 52, [0x38] = 53,
    [0x39] = 58, [0x3A] = 59, [0x3B] = 60, [0x3C] = 61,
    [0x3D] = 62, [0x3E] = 63, [0x3F] = 64, [0x40] = 65,
    [0x41] = 66, [0x42] = 67, [0x43] = 68, [0x44] = 87,
    [0x45] = 88, [0x46] = 99, [0x47] = 70, [0x48] = 119,
    [0x49] = 110, [0x4A] = 102, [0x4B] = 104, [0x4C] = 111,
    [0x4D] = 107, [0x4E] = 109, [0x4F] = 106, [0x50] = 105,
    [0x51] = 108, [0x52] = 103, [0x53] = 69, [0x54] = 98,
    [0x55] = 55, [0x56] = 74, [0x57] = 78, [0x58] = 96,
    [0x59] = 79, [0x5A] = 80, [0x5B] = 81, [0x5C] = 75,
    [0x5D] = 76, [0x5E] = 77, [0x5F] = 71, [0x60] = 72,
    [0x61] = 73, [0x62] = 82, [0x63] = 83, [0x64] = 86,
    [0x65] = 127, [0x66] = 116, [0x67] = 117, [0x68] = 183,
    [0x69] = 184, [0x6A] = 185, [0x6B] = 186, [0x6C] = 187,
    [0x6D] = 188, [0x6E] = 189, [0x6F] = 190, [0x70] = 191,
    [0x71] = 192, [0x72] = 193, [0x73] = 194, [0xE0] = 29,
    [0xE1] = 42, [0xE2] = 56, [0xE3] = 125, [0xE4] = 97,
    [0xE5] = 54, [0xE6] = 100, [0xE7] = 126,
};

static bool            g_raw_mouse_active = false;
static CFMachPortRef   g_event_tap = NULL;
static CFRunLoopSourceRef g_event_tap_src = NULL;
extern void libinput_sdl_inject_motion(double dx, double dy, uint64_t time_usec);
extern void libinput_sdl_inject_button(int button, int pressed, uint64_t time_usec);
extern void libinput_sdl_inject_axis(int axis, double value, uint64_t time_usec);
extern void libinput_sdl_inject_key(uint32_t scancode, int pressed, uint64_t time_usec);
static inline uint64_t now_usec(void);

static void show_mac_cursor(void) {
    if (!g_cursor_hidden) return;
    if (g_event_tap) CGEventTapEnable(g_event_tap, false);
    CGAssociateMouseAndMouseCursorPosition(true);
    uint32_t count = 0;
    CGDirectDisplayID displays[64];
    if (CGGetOnlineDisplayList(64, displays, &count) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < count; i++)
            CGDisplayShowCursor(displays[i]);
    }
    g_cursor_hidden = false;
}

static CGEventRef raw_mouse_tap_cb(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *refcon) {
    (void)proxy; (void)refcon;
    uint64_t t = now_usec();
    switch (type) {
        case kCGEventMouseMoved:
        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged: {
            int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
            int64_t dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);
            if (dx || dy) libinput_sdl_inject_motion((double)dx, (double)dy, t);
            break;
        }
        case kCGEventLeftMouseDown:
        case kCGEventLeftMouseUp:
        case kCGEventRightMouseDown:
        case kCGEventRightMouseUp:
        case kCGEventOtherMouseDown:
        case kCGEventOtherMouseUp: {
            int button = (int)CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
            int pressed = (type == kCGEventLeftMouseDown ||
                           type == kCGEventRightMouseDown ||
                           type == kCGEventOtherMouseDown);
            int evdev;
            switch (button) {
                case 0: evdev = 0x110; break;
                case 1: evdev = 0x112; break;
                case 2: evdev = 0x111; break;
                default: evdev = 0x110 + button; break;
            }
            libinput_sdl_inject_button(evdev, pressed, t);
            break;
        }
        case kCGEventScrollWheel: {
            double dx = CGEventGetDoubleValueField(event, kCGScrollWheelEventDeltaAxis2);
            double dy = CGEventGetDoubleValueField(event, kCGScrollWheelEventDeltaAxis1);
            if (dy != 0) libinput_sdl_inject_axis(0, dy, t);
            if (dx != 0) libinput_sdl_inject_axis(1, dx, t);
            break;
        }
        default: break;
    }
    return event;
}

static void *raw_mouse_tap_thread(void *p) {
    (void)p;
    if (g_event_tap_src)
        CFRunLoopAddSource(CFRunLoopGetCurrent(), g_event_tap_src, kCFRunLoopCommonModes);
    CFRunLoopRun();
    return NULL;
}

static void start_raw_mouse_capture(void) {
    if (g_raw_mouse_active) return;
    CGAssociateMouseAndMouseCursorPosition(false);
    CGEventMask mask =
        CGEventMaskBit(kCGEventMouseMoved) |
        CGEventMaskBit(kCGEventLeftMouseDown)   | CGEventMaskBit(kCGEventLeftMouseUp) |
        CGEventMaskBit(kCGEventRightMouseDown)  | CGEventMaskBit(kCGEventRightMouseUp) |
        CGEventMaskBit(kCGEventOtherMouseDown)  | CGEventMaskBit(kCGEventOtherMouseUp) |
        CGEventMaskBit(kCGEventLeftMouseDragged)| CGEventMaskBit(kCGEventRightMouseDragged) |
        CGEventMaskBit(kCGEventOtherMouseDragged) |
        CGEventMaskBit(kCGEventScrollWheel);
    g_event_tap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap,
                                   kCGEventTapOptionDefault, mask,
                                   raw_mouse_tap_cb, NULL);
    if (!g_event_tap) {
        fprintf(stderr, "[sdl] WARNING: could not create event tap (Accessibility permission needed). Falling back to SDL mouse.\n");
        CGAssociateMouseAndMouseCursorPosition(true);
        return;
    }
    g_event_tap_src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_event_tap, 0);
    pthread_t tid;
    pthread_create(&tid, NULL, raw_mouse_tap_thread, NULL);
    g_raw_mouse_active = true;
    fprintf(stderr, "[sdl] raw capture active (unconstrained pointer)\n");
}

static void hide_mac_cursor(void) {
    if (g_cursor_hidden) return;
    /* Allow hiding the cursor even when we are not the active app. */
    static bool bg_set = false;
    if (!bg_set) {
        bg_set = true;
        int cid = _CGSDefaultConnection();
        CFStringRef key = CFStringCreateWithCString(NULL, "SetsCursorInBackground",
                                                   kCFStringEncodingUTF8);
        CGSSetConnectionProperty(cid, cid, key, kCFBooleanTrue);
        CFRelease(key);
    }
    uint32_t count = 0;
    CGDirectDisplayID displays[64];
    if (CGGetOnlineDisplayList(64, displays, &count) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < count; i++)
            CGDisplayHideCursor(displays[i]);
    }
    g_cursor_hidden = true;
}

static void mac_cursor_restore_handler(int sig) {
    fprintf(stderr, "[sdl] signal %d: restoring macOS cursor\n", sig);
    show_mac_cursor();
    /* Re-raise with the default disposition so the process still exits. */
    signal(sig, SIG_DFL);
    raise(sig);
}

int g_space = 0;
long g_desktop_id = 0;

void WindowBecomeDesktop(NSWindow *w) {
    [w setLevel:CGWindowLevelForKey(kCGDesktopIconWindowLevel)];
    uint32_t wid = (uint32_t)[w windowNumber];
    int g_connection = SLSMainConnectionID();

    static uint64_t g_space = 0;

    if (!g_space) {
        g_space = SLSSpaceCreate(g_connection, 1, 0);
        SLSSpaceSetAbsoluteLevel(g_connection, g_space, 400);

        // Create CFNumberRef for space
        CFNumberRef space_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &g_space);
        CFArrayRef space_list = CFArrayCreate(kCFAllocatorDefault, (const void **)&space_num, 1, &kCFTypeArrayCallBacks);
        CFRelease(space_num);

        SLSShowSpaces(g_connection, space_list);
        CFRelease(space_list);
    }

    // Create CFNumberRef for window ID
    CFNumberRef window_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &wid);
    CFArrayRef window_list = CFArrayCreate(kCFAllocatorDefault, (const void **)&window_num, 1, &kCFTypeArrayCallBacks);
    CFRelease(window_num);

    SLSSpaceAddWindowsAndRemoveFromSpaces(g_connection,
                                          g_space,
                                          window_list,
                                          0x7);

    CFRelease(window_list);
}

#include <mach/mach_time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dispatch/dispatch.h>

#include "sdl_backend.h"

// Forward declare libinput injection (implemented in libinput.c)
extern void libinput_sdl_inject_key(uint32_t scancode, int pressed, uint64_t time_usec);
extern void libinput_sdl_inject_motion(double dx, double dy, uint64_t time_usec);
extern void libinput_sdl_inject_button(int button, int pressed, uint64_t time_usec);
extern void libinput_sdl_inject_axis(int axis, double value, uint64_t time_usec);
extern void libinput_sdl_inject_device_added(void);

// ── SDL state (main-thread only) ───────────────────────────────────────

// One SDL window/renderer per physical display (per CRTC). This is the only
// way to truly present a desktop across all macOS displays: a single window
// cannot span multiple Spaces/displays, so each display gets its own
// borderless desktop window positioned at that display's CG bounds.

static IOSurfaceRef g_pending_surface = NULL;
static pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_inited = false;
static volatile bool g_running = true;
static void *g_nswindow = NULL; // primary NSWindow* (first CRTC created)

// Multi-monitor per-CRTC state
#define MAX_CRTCS 16
typedef struct {
    uint32_t crtc_id;
    IOSurfaceRef surface;
    int x, y;        // Weston layout coords (informational)
    int w, h;
    bool dirty;
    bool has_window;
    SDL_Window   *window;
    void *nswindow;
    void *ca_layer;          // CALayer for zero-copy presentation
} crtc_state_t;
static crtc_state_t g_crtcs[MAX_CRTCS];
static int g_crtc_count = 0;
static pthread_mutex_t g_crtc_lock = PTHREAD_MUTEX_INITIALIZER;

// FPS tracking
static uint64_t g_fps_last[MAX_CRTCS];
static uint32_t g_fps_count[MAX_CRTCS];

// Map a CRTC id (1-based, as the virtual DRM assigns) to the CG display
// bounds (point coordinates in the global Cocoa/CG space). Uses the shared
// stable display snapshot so it always agrees with the DRM mode table.
static void crtc_display_bounds(uint32_t crtc_id, int *x, int *y, int *w, int *h) {
    int idx = (int)crtc_id - 1;
    if (idx < 0) idx = 0;
    mac_display_bounds(idx, x, y, w, h);
}

static crtc_state_t *get_or_create_crtc(uint32_t crtc_id) {
    pthread_mutex_lock(&g_crtc_lock);
    for (int i = 0; i < g_crtc_count; i++) {
        if (g_crtcs[i].crtc_id == crtc_id) {
            pthread_mutex_unlock(&g_crtc_lock);
            return &g_crtcs[i];
        }
    }
    if (g_crtc_count >= MAX_CRTCS) {
        pthread_mutex_unlock(&g_crtc_lock);
        return NULL;
    }
    crtc_state_t *c = &g_crtcs[g_crtc_count++];
    memset(c, 0, sizeof(*c));
    c->crtc_id = crtc_id;
    pthread_mutex_unlock(&g_crtc_lock);
    return c;
}

// Caller must hold g_crtc_lock.
static void ensure_crtc_window_locked(crtc_state_t *c) {
    if (c->has_window) return;
    int dx, dy, dw, dh;
    crtc_display_bounds(c->crtc_id, &dx, &dy, &dw, &dh);
    if (dw < 1) dw = 1280;
    if (dh < 1) dh = 720;

    SDL_Window *win = SDL_CreateWindow("wayland-mac framebuffer", dw, dh,
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_BORDERLESS);
    if (!win) {
        fprintf(stderr, "[sdl] CreateWindow crtc %u failed: %s\n", c->crtc_id, SDL_GetError());
        return;
    }
    // Position at the physical display's bounds in the global CG space.
    SDL_SetWindowPosition(win, dx, dy);
    // --- expose NSWindow handle right here (user request) ---
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    void *nsw = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (nsw) {
        WindowBecomeDesktop((NSWindow *)nsw);
    }
    SDL_ShowWindow(win);
    c->window = win;
    c->has_window = true;
    c->nswindow = nsw;
    if (c->crtc_id == 1 || !g_nswindow) g_nswindow = nsw;

    // Zero-copy CALayer path (no SDL renderer/texture)
    NSWindow *nswin = (NSWindow *)nsw;
    NSView *view = nswin ? [nswin contentView] : nil;
    if (view) {
        [view setWantsLayer:YES];
        CALayer *root = [view layer];
        CALayer *imgLayer = [CALayer layer];
        imgLayer.frame = root.bounds;
        imgLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        imgLayer.contentsGravity = kCAGravityTopLeft;
        imgLayer.contentsScale = 1.0;
        imgLayer.opaque = YES;
        [root addSublayer:imgLayer];
        c->ca_layer = (void *)CFBridgingRetain(imgLayer);
        fprintf(stderr, "[sdl] crtc %u: zero-copy CALayer ready\n", c->crtc_id);
    }

    fprintf(stderr, "[sdl] created window for crtc %u at %dx%d+%d+%d (nswindow %p)\n",
            c->crtc_id, dw, dh, dx, dy, nsw);
}

static inline uint64_t now_usec(void) {
    static mach_timebase_info_data_t tb = {0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t now = mach_absolute_time();
    return (now * tb.numer) / (tb.denom * 1000);
}

static inline bool is_main_thread(void) {
    return pthread_main_np() != 0;
}

// ── Rendering ──────────────────────────────────────────────────────────

// Caller must hold g_crtc_lock.
static void render_crtc_locked(crtc_state_t *c) {
    if (!c->surface) return;
    ensure_crtc_window_locked(c);
    if (!c->has_window) return;

    size_t w = IOSurfaceGetWidth(c->surface);
    size_t h = IOSurfaceGetHeight(c->surface);
    if (w == 0 || h == 0) return;

    int fidx = (int)c->crtc_id - 1;
    if (fidx >= 0 && fidx < MAX_CRTCS) g_fps_count[fidx]++;

    if (c->ca_layer) {
        CALayer *layer = (__bridge CALayer *)c->ca_layer;
        CGRect bounds = layer.superlayer.bounds;
        if (CGRectIsEmpty(bounds)) bounds = CGRectMake(0, 0, (CGFloat)w, (CGFloat)h);
        CGFloat neededScale = (bounds.size.width > 0) ? (CGFloat)w / bounds.size.width : 1.0;
        if (neededScale < 0.9) neededScale = 1.0;
        if (neededScale > 3.0) neededScale = 3.0;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.frame = bounds;
        layer.contentsScale = neededScale;
        layer.contents = (__bridge id)c->surface;
        [CATransaction commit];
        c->dirty = false;
        return;
    }

    c->dirty = false;
}

// Caller must hold g_crtc_lock.
static void render_dirty_crtcs_locked(void) {
    for (int i = 0; i < g_crtc_count; i++) {
        crtc_state_t *c = &g_crtcs[i];
        if (c->dirty || (c->surface && !c->has_window)) {
            render_crtc_locked(c);
        }
    }
}

// ── Input translation ──────────────────────────────────────────────────

static void handle_sdl_event(SDL_Event *ev) {
    uint64_t t = now_usec();
    switch (ev->type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            if (ev->key.repeat) break;
            SDL_Scancode sc = ev->key.scancode;
            int pressed = (ev->type == SDL_EVENT_KEY_DOWN) ? 1 : 0;
            uint32_t code = 0;
            if ((int)sc < 256) code = hid_to_evdev[sc];
            if (code == 0 && sc != SDL_SCANCODE_UNKNOWN) {
                fprintf(stderr, "[sdl] unmapped scancode %d (%s)\n", (int)sc, SDL_GetScancodeName(sc));
            } else if (code != 0) {
                libinput_sdl_inject_key(code, pressed, t);
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            if (g_raw_mouse_active) break;
            float dx = ev->motion.xrel;
            float dy = ev->motion.yrel;
            if (dx != 0 || dy != 0) {
                libinput_sdl_inject_motion((double)dx, (double)dy, t);
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (g_raw_mouse_active) break;
            int sdl_btn = ev->button.button;
            int evdev_btn = 0;
            switch (sdl_btn) {
                case SDL_BUTTON_LEFT:   evdev_btn = 0x110; break;
                case SDL_BUTTON_MIDDLE: evdev_btn = 0x112; break;
                case SDL_BUTTON_RIGHT:  evdev_btn = 0x111; break;
                case SDL_BUTTON_X1:     evdev_btn = 0x113; break;
                case SDL_BUTTON_X2:     evdev_btn = 0x114; break;
                default: evdev_btn = 0x110 + (sdl_btn - 1); break;
            }
            int pressed = (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? 1 : 0;
            libinput_sdl_inject_button(evdev_btn, pressed, t);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            if (g_raw_mouse_active) break;
            float wx = ev->wheel.x;
            float wy = ev->wheel.y;
            if (wy != 0) libinput_sdl_inject_axis(0, (double)wy, t);
            if (wx != 0) libinput_sdl_inject_axis(1, (double)wx, t);
            break;
        }
        default:
            break;
    }
}

// ── Public API ─────────────────────────────────────────────────────────

void sdl_backend_init(void) {
    if (g_inited) return;
    // Must be on main thread for Cocoa. If not, dispatch sync to main.
    if (!is_main_thread()) {
        dispatch_sync(dispatch_get_main_queue(), ^{
            sdl_backend_init();
        });
        return;
    }

    @autoreleasepool {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            fprintf(stderr, "[sdl] SDL_Init failed: %s\n", SDL_GetError());
            return;
        }

        // Windows are created lazily, one per CRTC/display, the first time a
        // surface is presented for that CRTC. This guarantees each physical
        // display gets its own desktop window.
        g_inited = true;
        g_running = true;
        libinput_sdl_inject_device_added();
        hide_mac_cursor();
        start_raw_mouse_capture();
        signal(SIGINT, mac_cursor_restore_handler);
        signal(SIGTERM, mac_cursor_restore_handler);
        atexit(show_mac_cursor);
        fprintf(stderr, "[sdl] SDL initialized (per-CRTC desktop windows created on demand)\n");
    }
}

void sdl_present_iosurface_for_crtc(IOSurfaceRef surface, uint32_t crtc_id, int x, int y) {
    if (!surface) return;
    if (!g_inited) {
        sdl_backend_init();
        if (!g_inited) {
            pthread_mutex_lock(&g_pending_lock);
            if (g_pending_surface) CFRelease(g_pending_surface);
            g_pending_surface = (IOSurfaceRef)CFRetain(surface);
            pthread_mutex_unlock(&g_pending_lock);
            return;
        }
    }
    crtc_state_t *c = get_or_create_crtc(crtc_id);
    if (!c) return;
    pthread_mutex_lock(&g_crtc_lock);
    if (c->surface) CFRelease(c->surface);
    c->surface = (IOSurfaceRef)CFRetain(surface);
    c->x = x; c->y = y;
    c->w = (int)IOSurfaceGetWidth(surface);
    c->h = (int)IOSurfaceGetHeight(surface);
    c->dirty = true;
    pthread_mutex_unlock(&g_crtc_lock);

    if (is_main_thread()) {
        pthread_mutex_lock(&g_crtc_lock);
        render_dirty_crtcs_locked();
        pthread_mutex_unlock(&g_crtc_lock);
    }
    // If not on the main thread, the next event pump will render it.
}

void sdl_present_iosurface(IOSurfaceRef surface) {
    // Legacy single-CRTC path (crtc 1 at 0,0)
    sdl_present_iosurface_for_crtc(surface, 1, 0, 0);
}

void sdl_backend_pump_events(void) {
    if (!g_inited) return;
    if (!is_main_thread()) {
        dispatch_sync(dispatch_get_main_queue(), ^{
            sdl_backend_pump_events();
        });
        return;
    }
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        handle_sdl_event(&ev);
    }

    // Render any CRTC surfaces that are dirty or don't have a window yet.
    bool any = false;
    pthread_mutex_lock(&g_crtc_lock);
    for (int i = 0; i < g_crtc_count; i++) {
        if (g_crtcs[i].dirty || (g_crtcs[i].surface && !g_crtcs[i].has_window)) any = true;
    }
    pthread_mutex_unlock(&g_crtc_lock);

    pthread_mutex_lock(&g_pending_lock);
    if (g_pending_surface) any = true;
    pthread_mutex_unlock(&g_pending_lock);

    if (any) {
        pthread_mutex_lock(&g_crtc_lock);
        render_dirty_crtcs_locked();
        pthread_mutex_unlock(&g_crtc_lock);

        // Drain any surface that arrived before init completed.
        pthread_mutex_lock(&g_pending_lock);
        IOSurfaceRef pending = g_pending_surface;
        g_pending_surface = NULL;
        pthread_mutex_unlock(&g_pending_lock);
        if (pending) {
            sdl_present_iosurface_for_crtc(pending, 1, 0, 0);
            CFRelease(pending);
        }
    }

    static uint64_t fps_last = 0;
    uint64_t now = now_usec();
    if (fps_last == 0) fps_last = now;
    if (now - fps_last >= 1000000) {
        uint64_t dt = now - fps_last;
        pthread_mutex_lock(&g_crtc_lock);
        for (int i = 0; i < g_crtc_count; i++) {
            uint32_t n = g_fps_count[i];
            int fps = (int)((uint64_t)n * 1000000 / dt);
            fprintf(stderr, "[sdl] crtc %u: %d fps\n", g_crtcs[i].crtc_id, fps);
            g_fps_count[i] = 0;
        }
        pthread_mutex_unlock(&g_crtc_lock);
        fps_last = now;
    }
}

void sdl_backend_shutdown(void) {
    if (!g_inited) return;
    show_mac_cursor();
    if (!is_main_thread()) {
        dispatch_sync(dispatch_get_main_queue(), ^{
            sdl_backend_shutdown();
        });
        return;
    }
    g_running = false;
    pthread_mutex_lock(&g_crtc_lock);
    for (int i = 0; i < g_crtc_count; i++) {
        crtc_state_t *c = &g_crtcs[i];
        if (c->ca_layer) { CFRelease(c->ca_layer); c->ca_layer = NULL; }
        if (c->window) { SDL_DestroyWindow(c->window); c->window = NULL; }
        if (c->surface) { CFRelease(c->surface); c->surface = NULL; }
        c->has_window = false;
        c->nswindow = NULL;
    }
    g_crtc_count = 0;
    pthread_mutex_unlock(&g_crtc_lock);
    g_nswindow = NULL;
    SDL_Quit();
    g_inited = false;
    pthread_mutex_lock(&g_pending_lock);
    if (g_pending_surface) { CFRelease(g_pending_surface); g_pending_surface = NULL; }
    pthread_mutex_unlock(&g_pending_lock);
}

void *sdl_get_nswindow(void) {
    return g_nswindow;
}
