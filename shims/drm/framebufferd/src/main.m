#include "drm_ipc.h"
#include "DisplaySurface.h"

#include <bootstrap.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
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

/* ── signal handler ───────────────────────────────────────────────────── */

static void handle_signal(int sig)
{
    (void)sig;
    g_running = false;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

/* ── 60 fps display timer ─────────────────────────────────────────────── */

static void TimerCallback(CFRunLoopTimerRef timer, void *info)
{
    (void)timer; (void)info;
    @autoreleasepool {
        if (!g_display || !g_display_surface) return;

        /* ── Lock and retain client surface ──────────────────────────── */
        pthread_mutex_lock(&g_surface_lock);
        IOSurfaceRef client = g_client_surface;
        if (client) CFRetain(client);
        pthread_mutex_unlock(&g_surface_lock);

        /* ── Lock cursor state ───────────────────────────────────────── */
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

        /* ── Blit client pixels into display surface ─────────────────── */
        size_t scw = IOSurfaceGetWidth(client);
        size_t sch = IOSurfaceGetHeight(client);
        size_t dw  = IOSurfaceGetWidth(g_display_surface);
        size_t dh  = IOSurfaceGetHeight(g_display_surface);
        size_t copy_w = scw < dw ? (size_t)scw : dw;
        size_t copy_h = sch < dh ? (size_t)sch : dh;

        IOSurfaceLock(client, 0, NULL);
        IOSurfaceLock(g_display_surface, 0, NULL);

        uint8_t *src_base = (uint8_t *)IOSurfaceGetBaseAddress(client);
        uint8_t *dst_base = (uint8_t *)IOSurfaceGetBaseAddress(g_display_surface);
        size_t src_stride = IOSurfaceGetBytesPerRow(client);
        size_t dst_stride = IOSurfaceGetBytesPerRow(g_display_surface);

        for (size_t y = 0; y < copy_h; y++)
            memcpy(dst_base + y * dst_stride, src_base + y * src_stride, copy_w * 4);

        /* ── Blend cursor onto display surface ───────────────────────── */
        if (cursor) {
            IOSurfaceLock(cursor, 0, NULL);
            uint8_t *cur_base = (uint8_t *)IOSurfaceGetBaseAddress(cursor);
            size_t cur_stride = IOSurfaceGetBytesPerRow(cursor);

            for (uint32_t y = 0; y < ch; y++) {
                int dy = cy + (int)y;
                if (dy < 0 || (size_t)dy >= dh) continue;
                for (uint32_t x = 0; x < cw; x++) {
                    int dx = cx + (int)x;
                    if (dx < 0 || (size_t)dx >= dw) continue;

                    uint32_t *cur_px = (uint32_t *)(cur_base + y * cur_stride + x * 4);
                    uint32_t *dst_px = (uint32_t *)(dst_base + (size_t)dy * dst_stride + (size_t)dx * 4);

                    uint32_t cp = *cur_px;
                    uint32_t dp = *dst_px;

                    uint8_t ca = (cp >> 24) & 0xFF;  /* ARGB -> A is MSB */
                    if (ca == 0) continue;

                    uint8_t cr = (cp >> 16) & 0xFF;
                    uint8_t cg = (cp >> 8)  & 0xFF;
                    uint8_t cb =  cp        & 0xFF;

                    uint8_t dr = (dp >> 16) & 0xFF;
                    uint8_t dg = (dp >> 8)  & 0xFF;
                    uint8_t db =  dp        & 0xFF;

                    uint8_t nr = (uint8_t)(((int)cr * ca + dr * (255 - ca)) / 255);
                    uint8_t ng = (uint8_t)(((int)cg * ca + dg * (255 - ca)) / 255);
                    uint8_t nb = (uint8_t)(((int)cb * ca + db * (255 - ca)) / 255);

                    *dst_px = (uint32_t)nb | ((uint32_t)ng << 8) | ((uint32_t)nr << 16) | ((uint32_t)0xFF << 24);
                }
            }
            IOSurfaceUnlock(cursor, 0, NULL);
            CFRelease(cursor);
        }

        IOSurfaceUnlock(g_display_surface, 0, NULL);
        IOSurfaceUnlock(client, 0, NULL);

        CFRelease(client);

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

            printf("[framebufferd] received surface=%p %zux%zu\n",
                   (void*)client_surface,
                   client_surface ? IOSurfaceGetWidth(client_surface) : 0,
                   client_surface ? IOSurfaceGetHeight(client_surface) : 0);
        }

        uint32_t len = msg.json_len < DRM_IPC_JSON_MAX
                     ? msg.json_len : DRM_IPC_JSON_MAX - 1;
        msg.json[len] = '\0';
        printf("[framebufferd] %s\n", msg.json);

        /* Route message by operation type */
        if (strstr(msg.json, "\"op\":\"cursor_set\"")) {
            /* Cursor set — store the cursor surface + dimensions */
            pthread_mutex_lock(&g_cursor_lock);
            if (g_cursor_surface) CFRelease(g_cursor_surface);
            g_cursor_surface = client_surface;
            g_cursor_w = 64; g_cursor_h = 64;
            /* Try to parse w/h from JSON */
            sscanf(msg.json, "%*[^w]\"w\":%u,\"h\":%u",
                   &g_cursor_w, &g_cursor_h);
            pthread_mutex_unlock(&g_cursor_lock);
        } else if (strstr(msg.json, "\"op\":\"cursor_move\"")) {
            /* Cursor move — update position */
            pthread_mutex_lock(&g_cursor_lock);
            sscanf(msg.json, "%*[^x]\"x\":%d,\"y\":%d",
                   &g_cursor_x, &g_cursor_y);
            pthread_mutex_unlock(&g_cursor_lock);
            if (client_surface) CFRelease(client_surface);
        } else if (client_surface) {
            /* Default: treat as page flip / surface update */
            pthread_mutex_lock(&g_surface_lock);
            if (g_client_surface) CFRelease(g_client_surface);
            g_client_surface = client_surface;
            pthread_mutex_unlock(&g_surface_lock);
        } else {
            if (client_surface) CFRelease(client_surface);
        }
    }
    return NULL;
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
        CGRect bounds = [g_display bounds];
        uint32_t dw = (uint32_t)CGRectGetWidth(bounds);
        uint32_t dh = (uint32_t)CGRectGetHeight(bounds);
        if (dw < 1) dw = 1920;
        if (dh < 1) dh = 1080;

        DisplaySurfaceInfo dsi = DisplaySurface_create(dw, dh, kWSPixelFormatBGRA);
        g_display_surface = dsi.surface;
        printf("[framebufferd] display surface %ux%u (%s)\n",
               dw, dh, g_display_surface ? "ok" : "FAILED");
        if (!g_display_surface) return 1;

        /* ── Start Mach server thread ───────────────────────────────── */
        pthread_t thread;
        pthread_create(&thread, NULL, mach_server_thread, NULL);
        pthread_detach(thread);

        /* ── 60 fps render loop ─────────────────────────────────────── */
        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
            kCFAllocatorDefault,
            CFAbsoluteTimeGetCurrent(),
            1.0 / 60.0,
            0, 0,
            TimerCallback,
            NULL);
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer,
                          kCFRunLoopCommonModes);

        printf("[framebufferd] render loop started\n");
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
