#include "glibc-compat.h"
#include <sys/cdefs.h>

/* funopen is a BSD/macOS extension; may be hidden by _POSIX_C_SOURCE */
FILE *funopen(const void *cookie,
              int (*readfn)(void *, char *, int),
              int (*writefn)(void *, const char *, int),
              fpos_t (*seekfn)(void *, fpos_t, int),
              int (*closefn)(void *));

/* Callback invoked by macOS stdio when flushing the FILE* stream.
 * cookie points to our wrapper struct:
 *   wrapper[0] = original user cookie
 *   wrapper[1..] = cookie_io_functions_t (function pointers)
 *
 * IMPORTANT: f->write must receive the ORIGINAL user cookie, not the wrapper. */
static int _fopencookie_write(void *cookie, const char *buf, int size) {
    void **wrapper = (void **)cookie;
    cookie_io_functions_t *f = (cookie_io_functions_t *)(wrapper + 1);
    return (int)f->write(wrapper[0], buf, (size_t)size);
}

static int _fopencookie_close(void *cookie) {
    void **wrapper = (void **)cookie;
    cookie_io_functions_t *f = (cookie_io_functions_t *)(wrapper + 1);
    int ret = f->close(wrapper[0]);
    free(wrapper);
    return ret;
}

FILE *fopencookie(void *cookie, const char *mode,
                  cookie_io_functions_t io_funcs) {
    (void)mode;
    void **wrapper = malloc(sizeof(void *) + sizeof(io_funcs));
    if (!wrapper) return NULL;
    wrapper[0] = cookie;
    memcpy(wrapper + 1, &io_funcs, sizeof(io_funcs));
    return funopen(wrapper, NULL, _fopencookie_write, NULL, _fopencookie_close);
}

/* ── udev input stubs (weston-internal, not in any shim) ─────────── */

struct udev_input;
struct udev_seat;
struct weston_compositor;
struct udev;

int udev_input_init(struct udev_input *input,
                    struct weston_compositor *c,
                    struct udev *udev,
                    const char *seat_id,
                    void *configure_device)
{
    (void)input;(void)c;(void)udev;(void)seat_id;(void)configure_device;
    return 0;
}

void udev_input_destroy(struct udev_input *input)
{
    (void)input;
}

int udev_input_enable(struct udev_input *input)
{
    (void)input;
    return 0;
}

void udev_input_disable(struct udev_input *input)
{
    (void)input;
}

struct udev_seat *udev_seat_get_named(struct udev_input *u,
                                       const char *seat_name)
{
    (void)u;(void)seat_name;
    return NULL;
}
