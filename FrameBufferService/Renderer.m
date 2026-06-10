//
//  Renderer.m
//  FrameBufferService
//
//  Composites a CALayer tree into a double-buffered IOSurface pair and
//  presents each frame to the CAWindowServer display.
//
//  Also hosts FBDevServer: a UNIX-domain socket server that exposes a
//  Linux-compatible /dev/fb0 interface.  The companion FBDevHook dylib
//  intercepts open() and ioctl() in client processes and redirects them
//  to this server transparently.
//
//  Rendering pipeline:
//    1. Create/resize a Metal texture that wraps the IOSurface (BGRA8).
//    2. Hand the texture to CARenderer (QuartzCore SPI).
//    3. Ask CARenderer to composite rootLayer into that texture.
//    4. Present the IOSurface to the display via -presentSurface:withOptions:.
//
//  FBDevServer pipeline:
//    1. Bind a UNIX-domain socket at FB_SOCKET_PATH (/tmp/fb0.sock).
//    2. Accept client connections on a private dispatch queue.
//    3. For each connection, serve FBRequest / FBResponse message pairs:
//         FBIOGET_VSCREENINFO / FBIOPUT_VSCREENINFO / FBIOGET_FSCREENINFO
//         FBIOPAN_DISPLAY / FBIOBLANK / pixel writes
//    4. Pixel writes are blitted into a dedicated CALayer sublayer so they
//       appear in the next rendered frame.
//

#import "Renderer.h"
#import "FBDevServer.h"
#import "DisplaySurface.h"

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// CAWindowServer display SPI (forward declarations only)
// ---------------------------------------------------------------------------

@interface NSObject (DWMDisplay)
- (CGRect)bounds;
- (void)presentSurface:(IOSurfaceRef)surface withOptions:(NSDictionary *)options;
@end

// ---------------------------------------------------------------------------
// CARenderer SPI  (system CARenderer.h provides the class already)
// ---------------------------------------------------------------------------

@interface CARenderer (DWMSPI)
+ (instancetype)rendererWithMTLTexture:(id)texture options:(NSDictionary *)options;
- (void)beginFrameAtTime:(CFTimeInterval)time timeStamp:(CVTimeStamp *)ts;
- (CGRect)updateBounds;
- (void)render;
- (void)endFrame;
@end

// ---------------------------------------------------------------------------
// Helpers: full read / write on a socket fd
// ---------------------------------------------------------------------------

static BOOL sock_read_fully(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return NO;
        p   += n;
        len -= (size_t)n;
    }
    return YES;
}

static BOOL sock_write_fully(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) return NO;
        p   += n;
        len -= (size_t)n;
    }
    return YES;
}

// ---------------------------------------------------------------------------
// FBDevServer – private interface
// ---------------------------------------------------------------------------

@interface FBDevServer ()
{
    int              _listenFd;
    dispatch_queue_t _serverQueue;      // accepts connections
    dispatch_queue_t _clientQueue;      // handles all client I/O
    BOOL             _running;

    // Weak back-reference to the renderer (not retained to avoid cycles)
    __weak id        _renderer;         // DWMRenderer *

    // Current virtual screen info (clients may update via PUT_VSCREENINFO)
    fb_var_screeninfo  _vinfo;
    pthread_mutex_t    _vinfoLock;

    // Pixel back-buffer: BGRA, sized to _vinfo.{xres, yres}
    // Written by clients, blitted into the IOSurface by blitToSurface:
    uint8_t         *_pixelBuf;
    size_t           _pixelBufSize;
    pthread_mutex_t  _pixelLock;
}
@end

// ---------------------------------------------------------------------------
// FBDevServer – shared singleton
// ---------------------------------------------------------------------------

@implementation FBDevServer

+ (instancetype)sharedServer
{
    static FBDevServer *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[FBDevServer alloc] init]; });
    return s;
}

- (instancetype)init
{
    self = [super init];
    if (!self) return nil;
    _listenFd = -1;
    pthread_mutex_init(&_vinfoLock, NULL);
    pthread_mutex_init(&_pixelLock, NULL);
    _serverQueue = dispatch_queue_create("com.cataracts.fbdev.server",
                                         DISPATCH_QUEUE_SERIAL);
    _clientQueue = dispatch_queue_create("com.cataracts.fbdev.clients",
                                         DISPATCH_QUEUE_CONCURRENT);
    return self;
}

// ---------------------------------------------------------------------------
// -startWithRenderer:
// ---------------------------------------------------------------------------

- (void)startWithRenderer:(id)renderer
{
    _renderer = renderer;

    // Remove any stale socket file.
    unlink(FB_SOCKET_PATH);

    _listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (_listenFd < 0) {
        fprintf(stderr, "[FBDev] socket() failed: %s\n", strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, FB_SOCKET_PATH, sizeof(addr.sun_path));

    if (bind(_listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[FBDev] bind() failed: %s\n", strerror(errno));
        close(_listenFd);
        _listenFd = -1;
        return;
    }

    // World-readable so unprivileged clients can connect.
    chmod(FB_SOCKET_PATH, 0666);

    if (listen(_listenFd, 8) < 0) {
        fprintf(stderr, "[FBDev] listen() failed: %s\n", strerror(errno));
        close(_listenFd);
        _listenFd = -1;
        return;
    }

    _running = YES;
    fprintf(stderr, "[FBDev] listening on %s\n", FB_SOCKET_PATH);

    // Accept loop on the server queue.
    int listenFd = _listenFd;
    __weak FBDevServer *weakSelf = self;
    dispatch_async(_serverQueue, ^{
        [weakSelf _acceptLoopOnFd:listenFd];
    });
}

- (void)stop
{
    _running = NO;
    if (_listenFd >= 0) {
        close(_listenFd);
        _listenFd = -1;
    }
    unlink(FB_SOCKET_PATH);
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------

- (void)_acceptLoopOnFd:(int)listenFd
{
    while (_running) {
        struct sockaddr_un clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(listenFd,
                              (struct sockaddr *)&clientAddr,
                              &clientLen);
        if (clientFd < 0) {
            if (_running)
                fprintf(stderr, "[FBDev] accept() failed: %s\n", strerror(errno));
            break;
        }

        fprintf(stderr, "[FBDev] client connected (fd=%d)\n", clientFd);

        __weak FBDevServer *weakSelf = self;
        dispatch_async(_clientQueue, ^{
            [weakSelf _serveClient:clientFd];
        });
    }
}

// ---------------------------------------------------------------------------
// Per-client request/response loop
// ---------------------------------------------------------------------------

- (void)_serveClient:(int)fd
{
    while (YES) {
        FBRequest req;
        if (!sock_read_fully(fd, &req, sizeof(req))) break;

        switch (req.op) {

        // ---------------------------------------------------------------
        case FB_OP_GET_VSCREENINFO: {
            pthread_mutex_lock(&_vinfoLock);
            fb_var_screeninfo vi = _vinfo;
            pthread_mutex_unlock(&_vinfoLock);

            FBResponse resp = { .status = 0,
                                .payloadSize = sizeof(vi) };
            sock_write_fully(fd, &resp, sizeof(resp));
            sock_write_fully(fd, &vi,   sizeof(vi));
            break;
        }

        // ---------------------------------------------------------------
        case FB_OP_PUT_VSCREENINFO: {
            fb_var_screeninfo vi;
            if ((size_t)req.payloadSize < sizeof(vi) ||
                !sock_read_fully(fd, &vi, sizeof(vi))) {
                FBResponse resp = { .status = -EINVAL, .payloadSize = 0 };
                sock_write_fully(fd, &resp, sizeof(resp));
                break;
            }
            [self _applyVarScreenInfo:&vi];
            FBResponse resp = { .status = 0, .payloadSize = 0 };
            sock_write_fully(fd, &resp, sizeof(resp));
            break;
        }

        // ---------------------------------------------------------------
        case FB_OP_GET_FSCREENINFO: {
            fb_fix_screeninfo fi = [self _buildFixScreenInfo];
            FBResponse resp = { .status = 0,
                                .payloadSize = sizeof(fi) };
            sock_write_fully(fd, &resp, sizeof(resp));
            sock_write_fully(fd, &fi,   sizeof(fi));
            break;
        }

        // ---------------------------------------------------------------
        case FB_OP_PAN_DISPLAY: {
            // Payload is a full fb_var_screeninfo; only xoffset/yoffset matter.
            fb_var_screeninfo vi;
            if ((size_t)req.payloadSize < sizeof(vi) ||
                !sock_read_fully(fd, &vi, sizeof(vi))) {
                FBResponse resp = { .status = -EINVAL, .payloadSize = 0 };
                sock_write_fully(fd, &resp, sizeof(resp));
                break;
            }
            pthread_mutex_lock(&_vinfoLock);
            _vinfo.xoffset = vi.xoffset;
            _vinfo.yoffset = vi.yoffset;
            pthread_mutex_unlock(&_vinfoLock);
            FBResponse resp = { .status = 0, .payloadSize = 0 };
            sock_write_fully(fd, &resp, sizeof(resp));
            break;
        }

        // ---------------------------------------------------------------
        case FB_OP_BLANK: {
            // Payload: 4-byte blank mode (0 = unblank, 1–4 = power-save levels)
            int32_t mode = 0;
            if (req.payloadSize >= sizeof(mode))
                sock_read_fully(fd, &mode, sizeof(mode));
            // No-op: we don't blank the display.
            (void)mode;
            FBResponse resp = { .status = 0, .payloadSize = 0 };
            sock_write_fully(fd, &resp, sizeof(resp));
            break;
        }

        // ---------------------------------------------------------------
        case FB_OP_WRITE_PIXELS: {
            FBPixelBlock blk;
            if ((size_t)req.payloadSize < sizeof(blk) ||
                !sock_read_fully(fd, &blk, sizeof(blk))) {
                FBResponse resp = { .status = -EINVAL, .payloadSize = 0 };
                sock_write_fully(fd, &resp, sizeof(resp));
                break;
            }
            size_t dataSize = (size_t)blk.stride * blk.h;
            uint8_t *pixels = malloc(dataSize);
            if (!pixels || !sock_read_fully(fd, pixels, dataSize)) {
                free(pixels);
                FBResponse resp = { .status = -ENOMEM, .payloadSize = 0 };
                sock_write_fully(fd, &resp, sizeof(resp));
                break;
            }
            int32_t status = [self _blitPixels:&blk data:pixels];
            free(pixels);
            FBResponse resp = { .status = status, .payloadSize = 0 };
            sock_write_fully(fd, &resp, sizeof(resp));
            break;
        }

        // ---------------------------------------------------------------
        case FB_OP_GET_FINFO_MAPSIZE: {
            pthread_mutex_lock(&_vinfoLock);
            uint32_t sz = (uint32_t)(_vinfo.xres_virtual *
                                      _vinfo.yres_virtual *
                                      (_vinfo.bits_per_pixel / 8));
            pthread_mutex_unlock(&_vinfoLock);
            FBResponse resp = { .status = 0, .payloadSize = sizeof(sz) };
            sock_write_fully(fd, &resp, sizeof(resp));
            sock_write_fully(fd, &sz, sizeof(sz));
            break;
        }

        // ---------------------------------------------------------------
        default: {
            // Drain any payload bytes so the stream stays in sync.
            if (req.payloadSize > 0) {
                uint8_t drain[256];
                uint32_t rem = req.payloadSize;
                while (rem > 0) {
                    uint32_t chunk = rem < sizeof(drain) ? rem : (uint32_t)sizeof(drain);
                    if (!sock_read_fully(fd, drain, chunk)) goto client_done;
                    rem -= chunk;
                }
            }
            FBResponse resp = { .status = -EINVAL, .payloadSize = 0 };
            sock_write_fully(fd, &resp, sizeof(resp));
            break;
        }

        } // switch
    } // while

client_done:
    fprintf(stderr, "[FBDev] client disconnected (fd=%d)\n", fd);
    close(fd);
}

// ---------------------------------------------------------------------------
// Apply a new fb_var_screeninfo
// ---------------------------------------------------------------------------

- (void)_applyVarScreenInfo:(const fb_var_screeninfo *)vi
{
    pthread_mutex_lock(&_vinfoLock);
    _vinfo = *vi;

    // Enforce BGRA8 regardless of what the client requests, since that is
    // what the IOSurface / Metal pipeline uses.
    _vinfo.bits_per_pixel  = 32;
    _vinfo.red             = (fb_bitfield){ 16, 8, 0 };  // BGRA: B@0 G@8 R@16 A@24
    _vinfo.green           = (fb_bitfield){  8, 8, 0 };
    _vinfo.blue            = (fb_bitfield){  0, 8, 0 };
    _vinfo.transp          = (fb_bitfield){ 24, 8, 0 };

    uint32_t w = _vinfo.xres_virtual ? _vinfo.xres_virtual : _vinfo.xres;
    uint32_t h = _vinfo.yres_virtual ? _vinfo.yres_virtual : _vinfo.yres;

    size_t needed = (size_t)w * h * 4;
    pthread_mutex_unlock(&_vinfoLock);

    pthread_mutex_lock(&_pixelLock);
    if (needed != _pixelBufSize) {
        free(_pixelBuf);
        _pixelBuf     = calloc(1, needed);
        _pixelBufSize = needed;
    }
    pthread_mutex_unlock(&_pixelLock);

    fprintf(stderr, "[FBDev] vinfo updated to %ux%u\n", w, h);
}

// ---------------------------------------------------------------------------
// Build fb_fix_screeninfo from current _vinfo
// ---------------------------------------------------------------------------

- (fb_fix_screeninfo)_buildFixScreenInfo
{
    pthread_mutex_lock(&_vinfoLock);
    uint32_t w   = _vinfo.xres_virtual ? _vinfo.xres_virtual : _vinfo.xres;
    uint32_t h   = _vinfo.yres_virtual ? _vinfo.yres_virtual : _vinfo.yres;
    uint32_t bpp = _vinfo.bits_per_pixel ? _vinfo.bits_per_pixel : 32;
    pthread_mutex_unlock(&_vinfoLock);

    fb_fix_screeninfo fi;
    memset(&fi, 0, sizeof(fi));
    strlcpy(fi.id, "FrameBufferSvc", sizeof(fi.id));
    fi.smem_start   = 0;
    fi.smem_len     = w * h * (bpp / 8);
    fi.type         = FB_TYPE_PACKED_PIXELS;
    fi.visual       = FB_VISUAL_TRUECOLOR;
    fi.line_length  = w * (bpp / 8);
    fi.xpanstep     = 0;
    fi.ypanstep     = 1;
    fi.ywrapstep    = 0;
    return fi;
}

// ---------------------------------------------------------------------------
// Blit a pixel block into _pixelBuf for later IOSurface blit.
// ---------------------------------------------------------------------------

- (int32_t)_blitPixels:(const FBPixelBlock *)blk data:(const uint8_t *)src
{
    pthread_mutex_lock(&_vinfoLock);
    uint32_t fw  = _vinfo.xres_virtual ? _vinfo.xres_virtual : _vinfo.xres;
    uint32_t fh  = _vinfo.yres_virtual ? _vinfo.yres_virtual : _vinfo.yres;
    pthread_mutex_unlock(&_vinfoLock);

    if (!fw || !fh) return -EINVAL;
    if (blk->x + blk->w > fw || blk->y + blk->h > fh) return -EINVAL;

    pthread_mutex_lock(&_pixelLock);

    if (!_pixelBuf) {
        _pixelBufSize = (size_t)fw * fh * 4;
        _pixelBuf = calloc(1, _pixelBufSize);
    }
    if (!_pixelBuf) {
        pthread_mutex_unlock(&_pixelLock);
        return -ENOMEM;
    }

    uint32_t rowBytes = fw * 4;
    for (uint32_t row = 0; row < blk->h; row++) {
        uint8_t *dst_row = _pixelBuf + (blk->y + row) * rowBytes + blk->x * 4;
        const uint8_t *src_row = src + row * blk->stride;
        memcpy(dst_row, src_row, (size_t)blk->w * 4);
    }

    pthread_mutex_unlock(&_pixelLock);

    fprintf(stderr, "[FBDev] blit %ux%u @ (%u,%u) fbuf=%ux%u\n",
            blk->w, blk->h, blk->x, blk->y, fw, fh);
    return 0;
}

// ---------------------------------------------------------------------------
// Blit the pixel buffer into an IOSurface.
// Called from the render thread AFTER CARenderer finishes compositing and
// the Metal command buffer completes, but BEFORE presentSurface:.
// ---------------------------------------------------------------------------

- (void)blitToSurface:(IOSurfaceRef)surface
{
    pthread_mutex_lock(&_pixelLock);
    if (!_pixelBuf || !_pixelBufSize) {
        pthread_mutex_unlock(&_pixelLock);
        return;
    }

    IOSurfaceLock(surface, 0, NULL);
    uint8_t *base = (uint8_t *)IOSurfaceGetBaseAddress(surface);
    size_t   dstBytesPerRow = IOSurfaceGetBytesPerRow(surface);
    size_t   srcBytesPerRow = IOSurfaceGetWidth(surface) * 4;
    size_t   h              = IOSurfaceGetHeight(surface);

    // Clamp to what we actually have
    pthread_mutex_lock(&_vinfoLock);
    size_t fw = _vinfo.xres_virtual ? _vinfo.xres_virtual : _vinfo.xres;
    size_t fh = _vinfo.yres_virtual ? _vinfo.yres_virtual : _vinfo.yres;
    pthread_mutex_unlock(&_vinfoLock);

    if (fw == 0 || fh == 0) { fw = (size_t)IOSurfaceGetWidth(surface); fh = (size_t)IOSurfaceGetHeight(surface); }

    size_t copyBytes = (fw * 4 < dstBytesPerRow) ? fw * 4 : dstBytesPerRow;
    size_t copyRows  = (fh < h) ? fh : h;

    for (size_t y = 0; y < copyRows; y++) {
        memcpy(base + y * dstBytesPerRow,
               _pixelBuf + y * srcBytesPerRow,
               copyBytes);
    }

    IOSurfaceUnlock(surface, 0, NULL);
    pthread_mutex_unlock(&_pixelLock);
}

// ---------------------------------------------------------------------------
// Called by DWMRenderer when the display size changes so the fbLayer tracks.
// ---------------------------------------------------------------------------

- (void)updateFrameToMatchDisplay:(CGRect)bounds
{
    // Also update vinfo to reflect the physical display size if the client
    // has not set an explicit resolution.
    pthread_mutex_lock(&_vinfoLock);
    if (_vinfo.xres == 0 || _vinfo.yres == 0) {
        _vinfo.xres         = (uint32_t)CGRectGetWidth(bounds);
        _vinfo.yres         = (uint32_t)CGRectGetHeight(bounds);
        _vinfo.xres_virtual = _vinfo.xres;
        _vinfo.yres_virtual = _vinfo.yres;
        _vinfo.bits_per_pixel = 32;
        _vinfo.red    = (fb_bitfield){ 16, 8, 0 };
        _vinfo.green  = (fb_bitfield){  8, 8, 0 };
        _vinfo.blue   = (fb_bitfield){  0, 8, 0 };
        _vinfo.transp = (fb_bitfield){ 24, 8, 0 };
    }
    pthread_mutex_unlock(&_vinfoLock);
}

@end


// ===========================================================================
// DWMRenderer
// ===========================================================================

// ---------------------------------------------------------------------------
// DWMRenderer (private interface)
// ---------------------------------------------------------------------------

@interface DWMRenderer ()
{
    id                   _display;          // CAWindowServer display

    id<MTLDevice>        _device;
    id<MTLCommandQueue>  _queue;

    // Double-buffered surfaces + Metal textures
    DisplaySurfaceInfo   _surfaces[2];
    id<MTLTexture>       _textures[2];
    CARenderer          *_caRenderers[2];
    int                  _bufferIndex;
}
@property (nonatomic, strong, readwrite) CALayer *rootLayer;
@end

// ---------------------------------------------------------------------------
// DWMRenderer (implementation)
// ---------------------------------------------------------------------------

@implementation DWMRenderer

- (instancetype)initWithDisplay:(id)display
{
    self = [super init];
    if (!self) return nil;

    _display = display;

    _device = MTLCreateSystemDefaultDevice();
    if (!_device) {
        fprintf(stderr, "[DWM] Metal unavailable\n");
        return nil;
    }
    _queue = [_device newCommandQueue];

    // Build the root layer.  Sublayers / content can be configured by the
    // caller via self.rootLayer after initialisation.
    _rootLayer = [CALayer layer];
    _rootLayer.frame = [_display bounds];
    _rootLayer.backgroundColor =
        CGColorCreateGenericRGB(0.7, 0.08, 0.16, 1.0);   // dark blue-grey

    // Start the /dev/fb0 server now that we have a rootLayer to attach to.
    [[FBDevServer sharedServer] startWithRenderer:self];

    fprintf(stderr, "[DWM] Renderer initialised (CALayer path)\n");
    return self;
}

// ---------------------------------------------------------------------------
// Per-frame surface / texture management
// ---------------------------------------------------------------------------

- (BOOL)_ensureSurfaceForWidth:(int)w height:(int)h index:(int)idx
{
    DisplaySurfaceInfo *dsi = &_surfaces[idx];
    if (dsi->surface && dsi->width == (uint32_t)w && dsi->height == (uint32_t)h)
        return YES;

    // Release the old CARenderer first (it may hold a reference to the texture)
    _caRenderers[idx] = nil;
    _textures[idx]    = nil;

    if (dsi->surface) DisplaySurface_destroy(dsi);

    *dsi = DisplaySurface_create((uint32_t)w, (uint32_t)h, kWSPixelFormatBGRA);
    if (!dsi->surface) {
        fprintf(stderr, "[DWM] DisplaySurface_create failed (%dx%d)\n", w, h);
        return NO;
    }

    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

    _textures[idx] = [_device newTextureWithDescriptor:td
                                             iosurface:dsi->surface
                                                 plane:0];
    if (!_textures[idx]) {
        fprintf(stderr, "[DWM] MTLTexture from IOSurface failed\n");
        DisplaySurface_destroy(dsi);
        return NO;
    }

    _caRenderers[idx] = [CARenderer rendererWithMTLTexture:_textures[idx] options:nil];
    _caRenderers[idx].layer  = _rootLayer;
    _caRenderers[idx].bounds = CGRectMake(0, 0, w, h);

    return YES;
}

// ---------------------------------------------------------------------------
// renderFrame
// ---------------------------------------------------------------------------

- (void)renderFrame
{
    CGRect bounds = [_display bounds];
    int w = (int)CGRectGetWidth(bounds);
    int h = (int)CGRectGetHeight(bounds);
    if (w < 1 || h < 1) return;

    int idx = _bufferIndex;

    if (![self _ensureSurfaceForWidth:w height:h index:idx]) return;

    // Keep the /dev/fb0 server in sync with the physical display geometry.
    [[FBDevServer sharedServer] updateFrameToMatchDisplay:bounds];

    // Update root layer geometry to match display
    _rootLayer.frame = CGRectMake(0, 0, w, h);

    // Composite via CARenderer
    CARenderer *renderer = _caRenderers[idx];
    CFTimeInterval now   = CACurrentMediaTime();

    [renderer beginFrameAtTime:now timeStamp:NULL];
    [renderer updateBounds];  // marks dirty region
    [renderer render];
    [renderer endFrame];

    // Flush the Metal command queue so CARenderer's GPU work is done
    id<MTLCommandBuffer> flush = [_queue commandBuffer];
    [flush commit];
    [flush waitUntilCompleted];

    // Blit the FBDevServer pixel buffer directly onto the IOSurface.
    // This runs after CARenderer finishes compositing, so the pixels
    // appear on top of the CALayer content.
    [[FBDevServer sharedServer] blitToSurface:_surfaces[idx].surface];

    // Present the IOSurface to the CAWindowServer display
    [_display presentSurface:_surfaces[idx].surface withOptions:@{}];

    _bufferIndex = !idx;
}

@end
