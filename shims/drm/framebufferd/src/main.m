#include "drm_ipc.h"
#include "DisplaySurface.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsTypes.h>  // has all the structs + SInt32 IOIndex


#include <bootstrap.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import <Accelerate/Accelerate.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <dlfcn.h>

#include <SymRez/SymRez.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

@interface NSObject (FBPDisplay)
- (void)presentSurface:(IOSurfaceRef)surface withOptions:(NSDictionary *)options;
- (CGRect)bounds;
@end

/* ── SkyLight / CoreDisplay SPI marker ────────────────────────────────── */

__attribute__((used, section("__SLSERVER,__slserver")))
static const char _slserver_marker = 1;

/* ── Fake session stubs required by SkyLight initialisation ──────────── */

static uint8_t  fake_session_data[0x200];
static uint8_t  fake_sub_object[0x200];
static uint8_t  fake_session_ctrl[0x100];
static uint8_t  fake_cursor_ctrl[0x100];
static uint64_t fake_connections_ptr;
static uint8_t *fake_event_data;
static uint8_t *fake_event_caps;

/* ── globals ──────────────────────────────────────────────────────────── */

static mach_port_t   g_server_port  = MACH_PORT_NULL;
static id            g_display;          /* CAWindowServerDisplay */

/* Our own display surface — created by DisplaySurface_create */
static IOSurfaceRef  g_display_surface;

/* The latest client surface (retained) — pixel source for blit */
static IOSurfaceRef  g_client_surface;

static pthread_mutex_t g_surface_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_running = true;

/* ── cursor state ─────────────────────────────────────────────────────── */

static IOSurfaceRef  g_cursor_surface;
static int           g_cursor_x, g_cursor_y;
static uint32_t      g_cursor_w, g_cursor_h;
static pthread_mutex_t g_cursor_lock = PTHREAD_MUTEX_INITIALIZER;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = false;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

/* ── Dirty tracking state ─────────────────────────────────────────────── */
static volatile bool g_dirty = true;

/* ── 60 fps display timer (Accelerate/vImage CPU compositing) ─────────── */

static void TimerCallback(CFRunLoopTimerRef timer, void *info)
{
    (void)timer; (void)info;
    @autoreleasepool {
        if (!g_display || !g_display_surface) return;

        if (!g_dirty) return;
        g_dirty = false;

        /* ── Snapshot client + cursor surfaces under lock ────────────── */
        pthread_mutex_lock(&g_surface_lock);
        IOSurfaceRef client = g_client_surface;
        if (client) CFRetain(client);
        pthread_mutex_unlock(&g_surface_lock);

        pthread_mutex_lock(&g_cursor_lock);
        IOSurfaceRef cursor = g_cursor_surface;
        if (cursor) CFRetain(cursor);
        int cx = g_cursor_x, cy = g_cursor_y;
        uint32_t cw = g_cursor_w, ch = g_cursor_h;
        pthread_mutex_unlock(&g_cursor_lock);

        if (!client) {
            if (cursor) CFRelease(cursor);
            return;
        }

        /* ── Accelerate / vImage Compositing ────────────────────────── */
        IOSurfaceLock(client, kIOSurfaceLockReadOnly, NULL);
        if (cursor) {
            IOSurfaceLock(cursor, kIOSurfaceLockReadOnly, NULL);
        }
        IOSurfaceLock(g_display_surface, 0, NULL);

        vImage_Buffer srcBuf = {
            .data = IOSurfaceGetBaseAddress(client),
            .width = IOSurfaceGetWidth(client),
            .height = IOSurfaceGetHeight(client),
            .rowBytes = IOSurfaceGetBytesPerRow(client)
        };
        vImage_Buffer dstBuf = {
            .data = IOSurfaceGetBaseAddress(g_display_surface),
            .width = IOSurfaceGetWidth(g_display_surface),
            .height = IOSurfaceGetHeight(g_display_surface),
            .rowBytes = IOSurfaceGetBytesPerRow(g_display_surface)
        };

        if (srcBuf.data && dstBuf.data) {
            if (srcBuf.width == dstBuf.width && srcBuf.height == dstBuf.height) {
                if (srcBuf.rowBytes == dstBuf.rowBytes) {
                    memcpy(dstBuf.data, srcBuf.data, srcBuf.rowBytes * srcBuf.height);
                } else {
                    size_t bytesToCopy = srcBuf.width * 4;
                    uint8_t *srcPtr = srcBuf.data;
                    uint8_t *dstPtr = dstBuf.data;
                    for (size_t y = 0; y < srcBuf.height; y++) {
                        memcpy(dstPtr, srcPtr, bytesToCopy);
                        srcPtr += srcBuf.rowBytes;
                        dstPtr += dstBuf.rowBytes;
                    }
                }
            } else {
                vImageScale_ARGB8888(&srcBuf, &dstBuf, NULL, kvImageNoFlags);
            }

            if (cursor) {
                vImage_Buffer cursorBuf = {
                    .data = IOSurfaceGetBaseAddress(cursor),
                    .width = IOSurfaceGetWidth(cursor),
                    .height = IOSurfaceGetHeight(cursor),
                    .rowBytes = IOSurfaceGetBytesPerRow(cursor)
                };

                if (cursorBuf.data) {
                    int dw_int = (int)dstBuf.width;
                    int dh_int = (int)dstBuf.height;

                    int x_min = cx;
                    int y_min = cy;
                    int x_max = cx + (int)cw;
                    int y_max = cy + (int)ch;

                    if (x_min < 0) x_min = 0;
                    if (y_min < 0) y_min = 0;
                    if (x_max > dw_int) x_max = dw_int;
                    if (y_max > dh_int) y_max = dh_int;

                    int intersect_w = x_max - x_min;
                    int intersect_h = y_max - y_min;

                    if (intersect_w > 0 && intersect_h > 0) {
                        uint8_t *dst_start = (uint8_t *)dstBuf.data + y_min * dstBuf.rowBytes + x_min * 4;
                        vImage_Buffer subDstBuf = {
                            .data = dst_start,
                            .width = intersect_w,
                            .height = intersect_h,
                            .rowBytes = dstBuf.rowBytes
                        };

                        int cursor_x_offset = x_min - cx;
                        int cursor_y_offset = y_min - cy;
                        uint8_t *cursor_start = (uint8_t *)cursorBuf.data + cursor_y_offset * cursorBuf.rowBytes + cursor_x_offset * 4;

                        vImage_Buffer subCursorBuf = {
                            .data = cursor_start,
                            .width = intersect_w,
                            .height = intersect_h,
                            .rowBytes = cursorBuf.rowBytes
                        };

                        vImagePremultipliedAlphaBlend_ARGB8888(
                            &subCursorBuf,
                            &subDstBuf,
                            &subDstBuf,
                            kvImageNoFlags
                        );
                    }
                }
            }
        }

        IOSurfaceUnlock(g_display_surface, 0, NULL);
        if (cursor) {
            IOSurfaceUnlock(cursor, kIOSurfaceLockReadOnly, NULL);
        }
        IOSurfaceUnlock(client, kIOSurfaceLockReadOnly, NULL);

        CFRelease(client);
        if (cursor) CFRelease(cursor);

        /* Present our display surface to the CAWindowServer */
        [g_display presentSurface:g_display_surface withOptions:@{}];
    }
}

/* ── Mach message server thread ────────────────────────────────────────── */

static void *mach_server_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        drm_ipc_msg_t msg = {0};
        msg.header.msgh_size       = sizeof(msg);
        msg.header.msgh_local_port = g_server_port;

        kern_return_t kr = mach_msg(&msg.header,
                                     MACH_RCV_MSG,
                                     0,
                                     sizeof(msg),
                                     g_server_port,
                                     MACH_MSG_TIMEOUT_NONE,
                                     MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            if (g_running)
                fprintf(stderr, "[framebufferd] mach_msg recv: %s\n",
                        mach_error_string(kr));
            continue;
        }

        if (msg.header.msgh_id != DRM_IPC_MSG_ID)
            continue;

        IOSurfaceRef client_surface = NULL;
        if (msg.body.msgh_descriptor_count >= 1 &&
            msg.surface_port.type == MACH_MSG_PORT_DESCRIPTOR &&
            msg.surface_port.name != MACH_PORT_NULL) {

            client_surface = IOSurfaceLookupFromMachPort(msg.surface_port.name);
            mach_port_deallocate(mach_task_self(), msg.surface_port.name);
        }

        /* Route message by operation type */
        if (strstr(msg.json, "\"op\":\"cursor_set\"")) {
            /* Cursor set — store the cursor surface + dimensions */
            pthread_mutex_lock(&g_cursor_lock);
            if (g_cursor_surface) CFRelease(g_cursor_surface);
            g_cursor_surface = client_surface;
            g_cursor_w = 64; g_cursor_h = 64;
            /* Try to parse w/h from JSON */
            sscanf(msg.json, "%*[^w]w\":%u,\"h\":%u",
                   &g_cursor_w, &g_cursor_h);
            g_dirty = true;
            pthread_mutex_unlock(&g_cursor_lock);
        } else if (strstr(msg.json, "\"op\":\"cursor_move\"")) {
            /* Cursor move — update position */
            pthread_mutex_lock(&g_cursor_lock);
            sscanf(msg.json, "%*[^x]x\":%d,\"y\":%d",
                   &g_cursor_x, &g_cursor_y);
            g_dirty = true;
            pthread_mutex_unlock(&g_cursor_lock);
            if (client_surface) CFRelease(client_surface);
        } else if (client_surface) {
            /* Default: treat as page flip / surface update */
            pthread_mutex_lock(&g_surface_lock);
            if (g_client_surface) CFRelease(g_client_surface);
            g_client_surface = client_surface;
            g_dirty = true;
            pthread_mutex_unlock(&g_surface_lock);
        } else {
            if (client_surface) CFRelease(client_surface);
        }
    }
    return NULL;
}

bool get_display_resolution(uint32_t *w, uint32_t *h) {
    NSString *path = @"/Library/Preferences/com.apple.windowserver.displays.plist";
    NSDictionary *plist = [NSDictionary dictionaryWithContentsOfFile:path];
    if (!plist) {
        fprintf(stderr, "[framebufferd] failed to read %s\n", [path UTF8String]);
        return false;
    }
    NSArray *configs = [plist valueForKeyPath:@"DisplayAnyUserSets.Configs"];
    if (![configs isKindOfClass:[NSArray class]]) return false;
    for (NSDictionary *cfg in configs) {
        NSArray *dispCfg = cfg[@"DisplayConfig"];
        if (![dispCfg isKindOfClass:[NSArray class]]) continue;
        for (NSDictionary *disp in dispCfg) {
            NSDictionary *info = disp[@"CurrentInfo"];
            if (![info isKindOfClass:[NSDictionary class]]) continue;
            uint32_t dw = [info[@"Wide"] unsignedIntValue];
            uint32_t dh = [info[@"High"] unsignedIntValue];
            if (dw > 0 && dh > 0) {
                *w = dw;
                *h = dh;
                return true;
            }
        }
    }
    fprintf(stderr, "[framebufferd] no display config found in plist\n");
    return false;
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    @autoreleasepool {
        /* ── Register Mach service ───────────────────────────────────── */
        kern_return_t kr = mach_port_allocate(mach_task_self(),
                                               MACH_PORT_RIGHT_RECEIVE,
                                               &g_server_port);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "[framebufferd] mach_port_allocate: %s\n",
                    mach_error_string(kr));
            return 1;
        }

        kr = mach_port_insert_right(mach_task_self(), g_server_port,
                                     g_server_port,
                                     MACH_MSG_TYPE_MAKE_SEND);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "[framebufferd] mach_port_insert_right: %s\n",
                    mach_error_string(kr));
            return 1;
        }

        kr = bootstrap_register(bootstrap_port, DRM_IPC_SERVICE_NAME,
                                 g_server_port);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "[framebufferd] bootstrap_register %s: %s\n",
                    DRM_IPC_SERVICE_NAME, mach_error_string(kr));
            return 1;
        }

        printf("[framebufferd] listening on %s\n", DRM_IPC_SERVICE_NAME);

        /* ── Set up CAWindowServer display pipeline ──────────────────── */

        /*
         * Mirror FrameBufferService exactly:
         *   a) dlopen private frameworks
         *   b) resolve ALL symbols upfront via SymRez
         *   c) call init functions unconditionally in order
         */

        /* a) Load private frameworks so SymRez can find them */
        dlopen("/System/Library/Frameworks/CoreDisplay.framework/Versions/A/CoreDisplay",
               RTLD_LAZY);
        dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight",
               RTLD_LAZY);
        dlopen("/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore",
               RTLD_LAZY);

        /* b) Resolve all symbols upfront */
#define SR_CD  "/System/Library/Frameworks/CoreDisplay.framework/Versions/A/CoreDisplay"
#define SR_SL  "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight"
#define SR_QC  "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore"

        symrez_t sr_cd = symrez_new(SR_CD);
        symrez_t sr_sl = symrez_new(SR_SL);
        symrez_t sr_qc = symrez_new(SR_QC);

        void  (*fn_SLSInit)(void)              = sr_resolve_symbol(sr_sl, "_SLSInitialize");
        void  (*fn_InitCD)(const void *)        = sr_resolve_symbol(sr_cd, "_InitializeCoreDisplay");
        void  (*fn_DispDrvInit)(void)           = sr_resolve_symbol(sr_cd, "_CGXDisplayDriverInitialize");
        void **p_WSCDCallbacks                  = sr_resolve_symbol(sr_sl, "_WSCDInitializeVtable.callbacks");
        void **p_sessionCtrl                    = sr_resolve_symbol(sr_sl, "___sessionControlRef");
        void **p_g_server                       = sr_resolve_symbol(sr_sl, "__ZL9_g_server");
        void  (*fn_CARenderServerRegister)(int) = sr_resolve_symbol(sr_sl, "_CARenderServerRegister");
        void **p_shared_server                  = sr_resolve_symbol(sr_qc, "__ZL14_shared_server");
        (void)fn_CARenderServerRegister;

        sr_free(sr_cd);
        sr_free(sr_sl);
        sr_free(sr_qc);

        /* c) Initialise SkyLight + CoreDisplay in order */
        fn_SLSInit();
        fn_InitCD(p_WSCDCallbacks);

        memset(fake_session_data, 0, sizeof(fake_session_data));
        memset(fake_sub_object,   0, sizeof(fake_sub_object));
        memset(fake_session_ctrl, 0, sizeof(fake_session_ctrl));
        memset(fake_cursor_ctrl,  0, sizeof(fake_cursor_ctrl));

        *(void    **)(fake_sub_object  + 232) = fake_session_data;
        *(void    **)(fake_sub_object  + 176) = &fake_connections_ptr;
        *(void    **)(fake_sub_object  + 256) = fake_cursor_ctrl;
        *(uint64_t *)(fake_cursor_ctrl + 120) = 0x10;
        *(void    **)(fake_session_ctrl + 32) = fake_sub_object;
        *p_sessionCtrl = fake_session_ctrl;

        fake_event_data = calloc(1, 0x1000);
        fake_event_caps = calloc(1, 0x100);
        *(void **)(fake_sub_object + 0xD0) = fake_event_data;
        *(void **)(fake_event_data + 0xA0) = fake_event_caps;

        fn_DispDrvInit();
        fprintf(stderr, "[framebufferd] CoreDisplay initialised\n");

        /* ── Create CAWindowServer instance ─────────────────────────── */
        Class caWS = NSClassFromString(@"CAWindowServer");
        if (p_shared_server) *p_shared_server = NULL;

        id server = ((id(*)(id, SEL, id))objc_msgSend)(
            (id)caWS,
            NSSelectorFromString(@"serverWithOptions:"),
            @{@"fetchFrozenSurfaces": @YES});

        if (p_g_server) *p_g_server = (__bridge void *)server;

        NSArray *displays = (NSArray *)[server performSelector:NSSelectorFromString(@"displays")];
        g_display = [displays firstObject];
        printf("[framebufferd] CAWindowServer ready, display=%s\n",
               g_display ? "yes" : "no");

        /* ── Create our own display surface ─────────────────────────── */

        uint32_t dw = 0, dh = 0;
        get_display_resolution(&dw, &dh);

        DisplaySurfaceInfo dsi = DisplaySurface_create(dw, dh, kWSPixelFormatBGRA);
        g_display_surface = dsi.surface;
        printf("[framebufferd] display surface %ux%u (%s)\n",
               dw, dh, g_display_surface ? "ok" : "FAILED");
        if (!g_display_surface) return 1;

        /* ── Start Mach server thread ───────────────────────────────── */
        pthread_t thread;
        pthread_create(&thread, NULL, mach_server_thread, NULL);
        pthread_detach(thread);

        /* ── Determine display refresh rate using CAWindowServerDisplay ── */
        double refreshRate = 60.0;
        if (g_display) {
            float idealRate = 0.0f;
            float maxRate = 0.0f;
            SEL selIdeal = NSSelectorFromString(@"idealRefreshRate");
            SEL selMax = NSSelectorFromString(@"maximumRefreshRate");

            if ([g_display respondsToSelector:selIdeal]) {
                idealRate = ((float (*)(id, SEL))objc_msgSend)(g_display, selIdeal);
            }
            if ([g_display respondsToSelector:selMax]) {
                maxRate = ((float (*)(id, SEL))objc_msgSend)(g_display, selMax);
            }

            if (idealRate > 0.0f) {
                refreshRate = idealRate;
            } else if (maxRate > 0.0f) {
                refreshRate = maxRate;
            } else {
                /* Default/Fallback for ProMotion or unknown display */
                refreshRate = 120.0;
            }
        }
        if (refreshRate < 60.0) {
            refreshRate = 60.0;
        }

        /* ── Render loop (synced to display refresh rate) ───────────── */
        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
            kCFAllocatorDefault,
            CFAbsoluteTimeGetCurrent(),
            1.0 / 120,
            0, 0,
            TimerCallback,
            NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer,
                          kCFRunLoopCommonModes);

        printf("[framebufferd] render loop started at %.1f FPS (Accelerate CPU compositing)\n", refreshRate);
        CFRunLoopRun();

        CFRelease(timer);
        g_running = false;

        pthread_mutex_lock(&g_surface_lock);
        if (g_client_surface) { CFRelease(g_client_surface); g_client_surface = NULL; }
        if (g_display_surface) { CFRelease(g_display_surface); g_display_surface = NULL; }
        pthread_mutex_unlock(&g_surface_lock);
    }
    return 0;
}
