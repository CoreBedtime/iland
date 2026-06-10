//
//  Hypervisor.m
//  Hypervisor
//
//  Entry point.  Bootstraps CoreDisplay and SkyLight, creates the
//  CAWindowServer, then drives the render loop via a CFRunLoopTimer.
//

#import <Foundation/Foundation.h>
#include <stdnoreturn.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
#import <dlfcn.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <SymRez/Core.h>

#import "Renderer.h"

// ---------------------------------------------------------------------------
// SkyLight / CoreDisplay SPI marker
// ---------------------------------------------------------------------------

__attribute__((used, section("__SLSERVER,__slserver")))
static const char _slserver_marker = 1;

// ---------------------------------------------------------------------------
// Fake session stubs required by SkyLight initialisation
// ---------------------------------------------------------------------------

static uint8_t  fake_session_data[0x200];
static uint8_t  fake_sub_object[0x200];
static uint8_t  fake_session_ctrl[0x100];
static uint8_t  fake_cursor_ctrl[0x100];
static uint64_t fake_connections_ptr;
static uint8_t *fake_event_data;
static uint8_t *fake_event_caps;

// ---------------------------------------------------------------------------
// Globals shared between main() and the timer callback
// ---------------------------------------------------------------------------

static id            g_server;       // CAWindowServer instance
static id            g_display;      // CAWindowServerDisplay for the target screen
static DWMRenderer  *g_renderer;     // CALayer-based renderer

// ---------------------------------------------------------------------------
// Timer callback  (~60 fps)
// ---------------------------------------------------------------------------

static void TimerCallback(CFRunLoopTimerRef timer, void *info)
{
    @autoreleasepool {
        [g_renderer renderFrame];
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void)
{
    @autoreleasepool {
        fprintf(stderr, "[DWM] starting (PID %d)\n", getpid());

        // ----------------------------------------------------------------
        // Load private frameworks
        // ----------------------------------------------------------------

        dlopen("/System/Library/Frameworks/CoreDisplay.framework/Versions/A/CoreDisplay",
               RTLD_LAZY);
        dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight",
               RTLD_LAZY);
        dlopen("/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore",
               RTLD_LAZY);

        // ----------------------------------------------------------------
        // Resolve symbols
        // ----------------------------------------------------------------

        symrez_t sr_cd = symrez_new(
            "/System/Library/Frameworks/CoreDisplay.framework/Versions/A/CoreDisplay");
        void (*fn_InitCD)(const void *)  = sr_resolve_symbol(sr_cd, "_InitializeCoreDisplay");
        void (*fn_DispDrvInit)(void)     = sr_resolve_symbol(sr_cd, "_CGXDisplayDriverInitialize");
        sr_free(sr_cd);

        symrez_t sr_sl = symrez_new(
            "/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight");
        void  (*fn_SLSInit)(void)              = sr_resolve_symbol(sr_sl, "_SLSInitialize");
        void **p_WSCDCallbacks                 = sr_resolve_symbol(sr_sl, "_WSCDInitializeVtable.callbacks");
        void **p_sessionCtrl                   = sr_resolve_symbol(sr_sl, "___sessionControlRef");
        void **p_g_server                      = sr_resolve_symbol(sr_sl, "__ZL9_g_server");
        void  (*fn_CARenderServerRegister)(int) = sr_resolve_symbol(sr_sl, "_CARenderServerRegister");
        (void)fn_CARenderServerRegister;       // reserved for future use
        sr_free(sr_sl);

        symrez_t sr_qc = symrez_new(
            "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore");
        void **p_shared_server = sr_resolve_symbol(sr_qc, "__ZL14_shared_server");
        sr_free(sr_qc);

        // ----------------------------------------------------------------
        // Initialise SkyLight + CoreDisplay
        // ----------------------------------------------------------------

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

        // Session event data  (required by WSEventProcessor annotate:)
        fake_event_data = calloc(1, 0x1000);
        fake_event_caps = calloc(1, 0x100);
        *(void **)(fake_sub_object + 0xD0) = fake_event_data;
        *(void **)(fake_event_data + 0xA0) = fake_event_caps;

        fn_DispDrvInit();
        fprintf(stderr, "[DWM] CoreDisplay initialised\n");

        // ----------------------------------------------------------------
        // Create CAWindowServer
        // ----------------------------------------------------------------

        Class caWS = NSClassFromString(@"CAWindowServer");
        if (p_shared_server) *p_shared_server = NULL;

        g_server = ((id(*)(id, SEL, id))objc_msgSend)(
            (id)caWS,
            NSSelectorFromString(@"serverWithOptions:"),
            @{@"fetchFrozenSurfaces": @YES});

        *p_g_server = (__bridge void *)g_server;

        // ----------------------------------------------------------------
        // Pick display  (last = external monitor if present)
        // ----------------------------------------------------------------

        NSArray *displays = (NSArray *)[g_server performSelector:NSSelectorFromString(@"displays")];
        NSLog(@"[DWM] displays -> %@", displays);

        BOOL useExternalDisplay = false;
        g_display = useExternalDisplay ? displays.lastObject : displays.firstObject;
        NSLog(@"[DWM] target display -> %@", g_display);

        // ----------------------------------------------------------------
        // Create renderer
        // ----------------------------------------------------------------

        g_renderer = [[DWMRenderer alloc] initWithDisplay:g_display];
        if (!g_renderer) {
            fprintf(stderr, "[DWM] failed to create renderer\n");
            return 1;
        }

        // ----------------------------------------------------------------
        // Run loop at ~60 fps
        // ----------------------------------------------------------------

        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
            kCFAllocatorDefault,
            CFAbsoluteTimeGetCurrent(),
            1.0 / 60.0,
            0, 0,
            TimerCallback,
            NULL);

        CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

        printf("[DWM] run loop started\n");
        CFRunLoopRun();

        CFRelease(timer);
    }
    return 0;
}
