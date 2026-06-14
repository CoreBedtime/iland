#include <metal_stdlib>
using namespace metal;

/// Composites a client framebuffer + optional cursor onto a display surface.
/// All three surfaces are BGRA8Unorm IOSurfaces bound as Metal textures.
///
/// The kernel reads from the client texture, alpha-blends the cursor on top
/// (if the cursor dimensions are non-zero), and writes the result to the
/// display texture.  The CPU does zero pixel work.
kernel void composite(texture2d<half, access::read>  client  [[texture(0)]],
                      texture2d<half, access::read>  cursor  [[texture(1)]],
                      texture2d<half, access::write> display [[texture(2)]],
                      constant int4 &cursorRect              [[buffer(0)]],
                      uint2 gid                              [[thread_position_in_grid]])
{
    const uint2 displaySize = uint2(display.get_width(), display.get_height());

    // Out-of-bounds guard
    if (gid.x >= displaySize.x || gid.y >= displaySize.y) return;

    // Read client pixel (clamp to client bounds, or black if out of range)
    const uint2 clientSize = uint2(client.get_width(), client.get_height());
    half4 pixel;
    if (gid.x < clientSize.x && gid.y < clientSize.y) {
        pixel = client.read(gid);
    } else {
        pixel = half4(0.0h, 0.0h, 0.0h, 1.0h);
    }

    // Alpha-blend cursor if the pixel falls within the cursor rect
    // cursorRect = { x, y, w, h }  in display coordinates
    int cx = cursorRect.x;
    int cy = cursorRect.y;
    int cw = cursorRect.z;
    int ch = cursorRect.w;

    if (cw > 0 && ch > 0) {
        int lx = int(gid.x) - cx;
        int ly = int(gid.y) - cy;
        if (lx >= 0 && ly >= 0 && lx < cw && ly < ch) {
            half4 cpx = cursor.read(uint2(uint(lx), uint(ly)));
            half  ca  = cpx.a;
            // Pre-multiplied alpha over
            pixel.rgb = cpx.rgb + pixel.rgb * (1.0h - ca);
            pixel.a   = 1.0h;
        }
    }

    display.write(pixel, gid);
}
