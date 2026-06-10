//
//  Renderer.h
//  Hypervisor
//
//  CALayer-based frame renderer.  One call to DWMRenderer_renderFrame()
//  composes the layer tree into the current IOSurface and presents it to
//  the CAWindowServer display.
//

#pragma once

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

/// Opaque renderer context.
@interface DWMRenderer : NSObject

/// The root CALayer whose contents are composited each frame.
@property (nonatomic, strong, readonly) CALayer *rootLayer;

/// Designated initialiser.  Pass the CAWindowServer display object
/// (rd_disp_ptr) so the renderer can query bounds and present surfaces.
- (instancetype)initWithDisplay:(id)display;

/// Render one frame: resize buffers if needed, composite rootLayer,
/// and present the result to the display.
- (void)renderFrame;

@end
