//
//  FBDevServer.h
//  FrameBufferService
//
//  Exposes a Linux-compatible /dev/fb0 interface over a UNIX-domain socket
//  at FB_SOCKET_PATH.  Applications that want to use the framebuffer should
//  link the FBDevHook dylib, which transparently redirects open("/dev/fb*")
//  and ioctl(fd, FBIO*) calls to this server.
//
//  Wire protocol (all messages are fixed-size, host byte order):
//
//    Client → Server   FBRequest
//    Server → Client   FBResponse
//
//  Each request/response pair completes one operation:
//
//    FB_OP_GET_VSCREENINFO   FBIOGET_VSCREENINFO  → fb_var_screeninfo
//    FB_OP_PUT_VSCREENINFO   FBIOPUT_VSCREENINFO  ← fb_var_screeninfo
//    FB_OP_GET_FSCREENINFO   FBIOGET_FSCREENINFO  → fb_fix_screeninfo
//    FB_OP_PAN_DISPLAY       FBIOPAN_DISPLAY      ← fb_var_screeninfo (xoffset/yoffset used)
//    FB_OP_BLANK             FBIOBLANK            ← int blank_mode
//    FB_OP_WRITE_PIXELS      write pixels into back-buffer   ← FBPixelBlock
//    FB_OP_GET_FINFO_MAPSIZE query total mmap size           → uint32_t
//

#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Socket path
// ---------------------------------------------------------------------------

#define FB_SOCKET_PATH  "/tmp/fb0.sock"

// ---------------------------------------------------------------------------
// Linux fb_fix_screeninfo  (linux/fb.h §3)
// ---------------------------------------------------------------------------

#define FB_TYPE_PACKED_PIXELS   0
#define FB_VISUAL_TRUECOLOR     2

typedef struct fb_fix_screeninfo {
    char        id[16];             // "FrameBufferSvc\0"
    uint64_t    smem_start;         // physical start (0 on macOS)
    uint32_t    smem_len;           // length of frame buffer mem
    uint32_t    type;               // FB_TYPE_*
    uint32_t    type_aux;
    uint32_t    visual;             // FB_VISUAL_*
    uint16_t    xpanstep;
    uint16_t    ypanstep;
    uint16_t    ywrapstep;
    uint32_t    line_length;        // bytes per display line
    uint64_t    mmio_start;         // 0
    uint32_t    mmio_len;           // 0
    uint32_t    accel;
    uint16_t    capabilities;
    uint16_t    reserved[2];
} __attribute__((packed)) fb_fix_screeninfo;

// ---------------------------------------------------------------------------
// Linux fb_bitfield / fb_var_screeninfo  (linux/fb.h §3)
// ---------------------------------------------------------------------------

typedef struct fb_bitfield {
    uint32_t    offset;
    uint32_t    length;
    uint32_t    msb_right;
} fb_bitfield;

typedef struct fb_var_screeninfo {
    uint32_t    xres;
    uint32_t    yres;
    uint32_t    xres_virtual;
    uint32_t    yres_virtual;
    uint32_t    xoffset;
    uint32_t    yoffset;
    uint32_t    bits_per_pixel;
    uint32_t    grayscale;
    fb_bitfield red;
    fb_bitfield green;
    fb_bitfield blue;
    fb_bitfield transp;
    uint32_t    nonstd;
    uint32_t    activate;
    uint32_t    height;             // mm
    uint32_t    width;              // mm
    uint32_t    accel_flags;
    uint32_t    pixclock;
    uint32_t    left_margin;
    uint32_t    right_margin;
    uint32_t    upper_margin;
    uint32_t    lower_margin;
    uint32_t    hsync_len;
    uint32_t    vsync_len;
    uint32_t    sync;
    uint32_t    vmode;
    uint32_t    rotate;
    uint32_t    colorspace;
    uint32_t    reserved[4];
} __attribute__((packed)) fb_var_screeninfo;

// ---------------------------------------------------------------------------
// Wire protocol
// ---------------------------------------------------------------------------

typedef enum : uint32_t {
    FB_OP_GET_VSCREENINFO  = 1,
    FB_OP_PUT_VSCREENINFO  = 2,
    FB_OP_GET_FSCREENINFO  = 3,
    FB_OP_PAN_DISPLAY      = 4,
    FB_OP_BLANK            = 5,
    FB_OP_WRITE_PIXELS     = 6,
    FB_OP_GET_FINFO_MAPSIZE = 7,
} FBOpCode;

typedef struct FBRequest {
    FBOpCode    op;
    uint32_t    payloadSize;    // bytes that follow this header (0 for GETs)
    // payload bytes follow immediately in the stream
} FBRequest;

typedef struct FBResponse {
    int32_t     status;         // 0 = success, -errno on error
    uint32_t    payloadSize;    // bytes that follow this header (0 for writes)
    // payload bytes follow immediately in the stream
} FBResponse;

// Payload for FB_OP_WRITE_PIXELS
typedef struct FBPixelBlock {
    uint32_t    x;
    uint32_t    y;
    uint32_t    w;
    uint32_t    h;
    uint32_t    stride;         // bytes per row in the data that follows
    // pixel data (stride * h bytes) follow immediately
} FBPixelBlock;

// ---------------------------------------------------------------------------
// Objective-C class (FrameBufferService side)
// ---------------------------------------------------------------------------

#ifdef __OBJC__

#import <Foundation/Foundation.h>

/// Manages the /dev/fb0 UNIX-socket server.
///
/// Start with -[FBDevServer startWithRenderer:] after the DWMRenderer is
/// ready.  The server runs on a private dispatch queue and responds to client
/// connections on demand.
///
/// Pixel writes from clients are accumulated in a private BGRA buffer.
/// Each frame, DWMRenderer calls -blitToSurface: to copy the buffer directly
/// into the IOSurface after CARenderer finishes compositing, so the pixels
/// appear on top of the CALayer tree.
@interface FBDevServer : NSObject

/// Shared singleton.
+ (instancetype)sharedServer;

/// Bind the socket and begin accepting clients.
/// @param renderer   The live DWMRenderer whose rootLayer will receive pixel data.
- (void)startWithRenderer:(id)renderer;

/// Stop accepting and tear down.
- (void)stop;

@end

#endif // __OBJC__
