#import <gbm_priv.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <stdlib.h>
#import <string.h>

static IOSurfaceRef create_iosurface(uint32_t width, uint32_t height,
                                      uint32_t format)
{
    (void)format;
    uint32_t stride = width * 4;
    size_t total = stride * height;

    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    [dict setObject:@(width)       forKey:(id)kIOSurfaceWidth];
    [dict setObject:@(height)      forKey:(id)kIOSurfaceHeight];
    [dict setObject:@((int)'BGRA') forKey:(id)kIOSurfacePixelFormat];
    [dict setObject:@(stride)      forKey:(id)kIOSurfaceBytesPerRow];
    [dict setObject:@(total)       forKey:(id)kIOSurfaceAllocSize];

    return IOSurfaceCreate((CFDictionaryRef)dict);
}

struct gbm_device *gbm_create_device(int fd)
{
    struct gbm_device *dev = calloc(1, sizeof(*dev));
    if (!dev) return NULL;
    dev->fd = fd;
    dev->refcount = 1;
    return dev;
}

void gbm_device_destroy(struct gbm_device *gbm)
{
    if (!gbm) return;
    if (--gbm->refcount > 0) return;
    free(gbm);
}

struct gbm_surface *gbm_surface_create(struct gbm_device *gbm,
                                        uint32_t width, uint32_t height,
                                        uint32_t format, uint32_t flags)
{
    struct gbm_surface *surf = calloc(1, sizeof(*surf));
    if (!surf) return NULL;

    surf->device = gbm;
    surf->width  = width;
    surf->height = height;
    surf->format = format;
    surf->flags  = flags;

    for (int i = 0; i < GBM_NUM_BUFFERS; i++) {
        struct gbm_bo *bo = calloc(1, sizeof(*bo));
        if (!bo) goto fail;
        bo->device = gbm;
        bo->width  = width;
        bo->height = height;
        bo->stride = width * 4;
        bo->format = format;
        bo->surface = create_iosurface(width, height, format);
        if (!bo->surface) { free(bo); goto fail; }
        surf->bos[i] = bo;
    }
    return surf;

fail:
    for (int i = 0; i < GBM_NUM_BUFFERS; i++) {
        if (surf->bos[i]) {
            if (surf->bos[i]->surface)
                CFRelease(surf->bos[i]->surface);
            free(surf->bos[i]);
        }
    }
    free(surf);
    return NULL;
}

void gbm_surface_destroy(struct gbm_surface *surface)
{
    if (!surface) return;
    for (int i = 0; i < GBM_NUM_BUFFERS; i++) {
        if (surface->bos[i]) {
            if (surface->bos[i]->surface)
                CFRelease(surface->bos[i]->surface);
            free(surface->bos[i]);
        }
    }
    free(surface);
}

struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surface)
{
    if (!surface || surface->count == 0) return NULL;
    struct gbm_bo *bo = surface->bos[surface->read_idx];
    surface->read_idx = (surface->read_idx + 1) % GBM_NUM_BUFFERS;
    surface->count--;
    return bo;
}

void gbm_surface_release_buffer(struct gbm_surface *surface,
                                 struct gbm_bo *bo)
{
    (void)surface;
    (void)bo;
}

IOSurfaceRef gbm_bo_get_iosurface(struct gbm_bo *bo)
{
    return bo ? bo->surface : NULL;
}

struct gbm_bo *gbm_surface_get_write_bo(struct gbm_surface *surface)
{
    if (!surface) return NULL;
    return surface->bos[surface->write_idx];
}

void gbm_surface_advance_write(struct gbm_surface *surface)
{
    if (!surface) return;
    surface->write_idx = (surface->write_idx + 1) % GBM_NUM_BUFFERS;
    surface->count++;
}

uint32_t gbm_bo_get_width(struct gbm_bo *bo)
{
    return bo ? bo->width : 0;
}

uint32_t gbm_bo_get_height(struct gbm_bo *bo)
{
    return bo ? bo->height : 0;
}

uint32_t gbm_bo_get_stride(struct gbm_bo *bo)
{
    return bo ? bo->stride : 0;
}

uint32_t gbm_bo_get_format(struct gbm_bo *bo)
{
    return bo ? bo->format : 0;
}

struct gbm_device *gbm_bo_get_device(struct gbm_bo *bo)
{
    return bo ? bo->device : NULL;
}

union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo)
{
    union gbm_bo_handle h = {0};
    if (bo && bo->surface)
        h.u32 = (uint32_t)IOSurfaceGetID(bo->surface);
    return h;
}

void gbm_bo_set_user_data(struct gbm_bo *bo, void *data,
                           void (*destroy)(struct gbm_bo *, void *))
{
    if (!bo) return;
    bo->user_data = data;
    bo->destroy_user_data = destroy;
}

void *gbm_bo_get_user_data(struct gbm_bo *bo)
{
    return bo ? bo->user_data : NULL;
}
