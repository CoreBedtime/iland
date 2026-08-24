#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/select.h>
#include <sys/time.h>

/* DRM ioctl dispatch — intercepts open/ioctl for /dev/dri/card* */
#include <sys/ioctl.h>
#include "drm_ioctl.h"
#include "sdl_backend.h"

/* epoll shim functions we hook into — forward-declared to avoid pulling
 * in epoll_shim_ctx.h and its system-compat dependencies */
ssize_t epoll_shim_read(int fd, void *buf, size_t nbytes);
ssize_t epoll_shim_write(int fd, void const *buf, size_t nbytes);
int     epoll_shim_close(int fd);
int     epoll_shim_poll(struct pollfd fds[], nfds_t nfds, int timeout);
int     epoll_shim_fcntl(int fd, int cmd, ...);

/* ── Dobby hook function pointer definitions ──────────────────────────
 * These are referenced via `extern` by wrap.c in epoll-shim-interpose.
 * DobbyHook is forward-declared to avoid pulling in <dobby.h> here. */

int DobbyHook(void *function_address, void *replace_call, void **origin_call);

typeof(read)  *wrap_real_read;
typeof(write) *wrap_real_write;
typeof(close) *wrap_real_close;
typeof(poll)  *wrap_real_poll;
typeof(fcntl) *wrap_real_fcntl;
typeof(open)  *wrap_real_open;
typeof(ioctl) *wrap_real_ioctl;
typeof(select) *wrap_real_select;
typeof(kevent) *wrap_real_kevent;
typeof(kevent64) *wrap_real_kevent64;

static ssize_t hooked_read(int fd, void *buf, size_t nbytes)
{
    return epoll_shim_read(fd, buf, nbytes);
}

static ssize_t hooked_write(int fd, void const *buf, size_t nbytes)
{
    return epoll_shim_write(fd, buf, nbytes);
}

static int hooked_close(int fd)
{
    if (fd == DRM_VIRTUAL_FD)
        return 0;
    return epoll_shim_close(fd);
}

static int hooked_poll(struct pollfd fds[], nfds_t nfds, int timeout)
{
    // Pump SDL events on main thread before blocking. This ensures keyboard/mouse
    // events from the SDL window are translated to libinput events.
    sdl_backend_pump_events();
    // Cap timeout to 16ms so SDL events are polled at ~60Hz even when weston blocks with -1
    int capped = timeout;
    if (capped < 0 || capped > 16) capped = 16;
    int ret = epoll_shim_poll(fds, nfds, capped);
    // Pump again after wake to catch events that arrived during poll
    sdl_backend_pump_events();
    // If we capped timeout but original was longer, and we got no events, simulate timeout behavior
    if (ret == 0 && capped != timeout) {
        // If original timeout was infinite or longer, don't return 0 prematurely if no fds ready?
        // But SDL pump needs frequent wakeups, so we return 0 to let caller loop and check again.
        // For correctness, if caller expects -1 (infinite) we still return 0 with no events, which will cause busy loop but at 60Hz it's okay.
    }
    return ret;
}

static int hooked_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    sdl_backend_pump_events();
    // Cap timeout to 16ms
    struct timeval capped_tv;
    struct timeval *capped_ptr = timeout;
    if (timeout) {
        capped_tv = *timeout;
        if (capped_tv.tv_sec > 0 || capped_tv.tv_usec > 16000) {
            capped_tv.tv_sec = 0;
            capped_tv.tv_usec = 16000;
            capped_ptr = &capped_tv;
        }
    } else {
        capped_tv.tv_sec = 0;
        capped_tv.tv_usec = 16000;
        capped_ptr = &capped_tv;
    }
    int ret = wrap_real_select(nfds, readfds, writefds, exceptfds, capped_ptr);
    sdl_backend_pump_events();
    return ret;
}

static int hooked_kevent(int kq, const struct kevent *changelist, int nchanges,
                         struct kevent *eventlist, int nevents,
                         const struct timespec *timeout)
{
    sdl_backend_pump_events();
    struct timespec capped_ts;
    const struct timespec *capped_ptr = timeout;
    if (!timeout) {
        capped_ts.tv_sec = 0;
        capped_ts.tv_nsec = 16 * 1000000;
        capped_ptr = &capped_ts;
    } else if (timeout->tv_sec > 0 || timeout->tv_nsec > 16 * 1000000) {
        capped_ts.tv_sec = 0;
        capped_ts.tv_nsec = 16 * 1000000;
        capped_ptr = &capped_ts;
    }
    int ret = wrap_real_kevent(kq, changelist, nchanges, eventlist, nevents, capped_ptr);
    sdl_backend_pump_events();
    return ret;
}

static int hooked_kevent64(int kq, const struct kevent64_s *changelist, int nchanges,
                           struct kevent64_s *eventlist, int nevents,
                           unsigned int flags, const struct timespec *timeout)
{
    sdl_backend_pump_events();
    struct timespec capped_ts;
    const struct timespec *capped_ptr = timeout;
    if (!timeout) {
        capped_ts.tv_sec = 0;
        capped_ts.tv_nsec = 16 * 1000000;
        capped_ptr = &capped_ts;
    } else if (timeout->tv_sec > 0 || timeout->tv_nsec > 16 * 1000000) {
        capped_ts.tv_sec = 0;
        capped_ts.tv_nsec = 16 * 1000000;
        capped_ptr = &capped_ts;
    }
    int ret = wrap_real_kevent64(kq, changelist, nchanges, eventlist, nevents, flags, capped_ptr);
    sdl_backend_pump_events();
    return ret;
}

static int hooked_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    int rv = epoll_shim_fcntl(fd, cmd, arg);
    va_end(ap);
    return rv;
}

/* ── DRM open/ioctl hooks ───────────────────────────────────────────── */

static int hooked_open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);

    if (path && strncmp(path, "/dev/dri/card", 13) == 0) {
        const char *rest = path + 13;
        if (*rest >= '0' && *rest <= '9')
            return DRM_VIRTUAL_FD;
    }
    return wrap_real_open(path, flags, mode);
}

static int hooked_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (fd == DRM_VIRTUAL_FD)
        return drm_ioctl_dispatch(request, arg);
    return wrap_real_ioctl(fd, request, arg);
}

static void install_epoll_hooks(void)
{
#define HOOK(fun)                                                         \
    do {                                                                  \
        int ret = DobbyHook((void *)fun, (void *)hooked_##fun,            \
                            (void **)&wrap_real_##fun);                   \
        if (ret != 0) {                                                   \
            fprintf(stderr,                                               \
                    "epoll-shim: error hooking \"" #fun "\" with DobbyHook!\n"); \
            abort();                                                      \
        }                                                                 \
    } while (0)
#define HOOK_OPT(fun)                                                     \
    do {                                                                  \
        int ret = DobbyHook((void *)fun, (void *)hooked_##fun,            \
                            (void **)&wrap_real_##fun);                   \
        if (ret != 0) {                                                   \
            fprintf(stderr,                                               \
                    "[wayland-mac] optional hook \"" #fun "\" not found (%d), skipping\n", ret); \
        }                                                                 \
    } while (0)

    HOOK(read);
    HOOK(write);
    HOOK(close);
    HOOK(poll);
    HOOK(fcntl);
    HOOK_OPT(select);
    HOOK_OPT(kevent);
    HOOK_OPT(kevent64);

#undef HOOK
#undef HOOK_OPT
}

static void install_drm_hooks(void)
{
    int ret;

    ret = DobbyHook((void *)open,      (void *)hooked_open,
                    (void **)&wrap_real_open);
    if (ret != 0) {
        fprintf(stderr, "wayland-mac: error hooking \"open\" with DobbyHook!\n");
        abort();
    }

    ret = DobbyHook((void *)ioctl,     (void *)hooked_ioctl,
                    (void **)&wrap_real_ioctl);
    if (ret != 0) {
        fprintf(stderr, "wayland-mac: error hooking \"ioctl\" with DobbyHook!\n");
        abort();
    }
}

__attribute__((constructor))
static void wayland_mac_load(void) {
    // In SDL single-library mode we no longer require root; AMFI patching removed.
    if (getuid() != 0) {
        fprintf(stderr, "[wayland-mac] running as uid %d (non-root SDL mode)\n", getuid());
    }

    /* Create a real pipe dup'd to DRM_VIRTUAL_FD so select/poll work on
     * our virtual DRM fd.  The read end becomes fd 42; the write end is
     * stored for drmModePageFlip to signal page-flip completion events. */
    {
        int p[2];
        if (pipe(p) == 0) {
            dup2(p[0], DRM_VIRTUAL_FD);
            close(p[0]);
            /* Declared in drm_linux.h, defined in drm_linux.c */
            extern int g_drm_event_pipe_write;
            g_drm_event_pipe_write = p[1];
        }
    }

    /* Install hooks before anything else — these intercept libc calls
     * (read, write, poll, close, fcntl) and route them through the
     * epoll shim. */
    install_epoll_hooks();
    install_drm_hooks();

    // SDL backend is now lazy-initialised on first DRM page-flip or libinput
    // context creation. This prevents extra SDL windows in wayland clients
    // (weston-terminal, etc.) that also load libwayland-mac but never use DRM.
    fprintf(stderr, "[wayland-mac] SDL single-library mode ready (lazy init, no daemons)\n");
}

__attribute__((visibility("default")))
void wayland_mac_init(void) {
    (void)0;
}
