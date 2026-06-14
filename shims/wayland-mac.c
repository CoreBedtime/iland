#include <_abort.h>
#include <_stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <servers/bootstrap.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdarg.h>
#include <poll.h>
#include <signal.h>

/* DRM ioctl dispatch — intercepts open/ioctl for /dev/dri/card* */
#include <sys/ioctl.h>
#include "drm_ioctl.h"

/* epoll shim functions we hook into — forward-declared to avoid pulling
 * in epoll_shim_ctx.h and its system-compat dependencies */
ssize_t epoll_shim_read(int fd, void *buf, size_t nbytes);
ssize_t epoll_shim_write(int fd, void const *buf, size_t nbytes);
int     epoll_shim_close(int fd);
int     epoll_shim_poll(struct pollfd fds[], nfds_t nfds, int timeout);
int     epoll_shim_fcntl(int fd, int cmd, ...);

#define SUPPORT_DIR "/tmp/libwayland-support"

extern char **environ;

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
    return epoll_shim_poll(fds, nfds, timeout);
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

    HOOK(read);
    HOOK(write);
    HOOK(close);
    HOOK(poll);
    HOOK(fcntl);

#undef HOOK
}

static int extract_section(const char *segname, const char *sectname,
                            const char *destpath) {
    unsigned long size = 0;
    const uint8_t *data = getsectiondata(&_mh_dylib_header, segname, sectname,
                                         &size);
    if (!data || size == 0) {
        fprintf(stderr, "[wayland-mac] section %s,%s not found\n",
                segname, sectname);
        return -1;
    }

    int fd = open(destpath, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        fprintf(stderr, "[wayland-mac] open %s: %s\n", destpath,
                strerror(errno));
        return -1;
    }

    ssize_t written = write(fd, data, size);
    close(fd);

    if (written != (ssize_t)size) {
        fprintf(stderr, "[wayland-mac] write %s: short write\n", destpath);
        return -1;
    }

    return 0;
}

/* Build environment without DYLD_INSERT_LIBRARIES so spawned children
 * don't recursively load our dylib. */
static char **clean_environ(void) {
    static char **clean = NULL;
    if (clean) return clean;

    int count = 0;
    while (environ[count]) count++;

    /* Each entry survives unless it starts with DYLD_INSERT_LIBRARIES=.
     * Worst case: all survive. */
    clean = calloc(count + 1, sizeof(char *));
    if (!clean) return environ;

    int j = 0;
    for (int i = 0; i < count; i++) {
        if (strncmp(environ[i], "DYLD_INSERT_LIBRARIES=", 22) == 0)
            continue;
        clean[j++] = environ[i];
    }
    clean[j] = NULL;

    unsetenv("DYLD_INSERT_LIBRARIES");

    return clean;
}

#define POSIX_SPAWN_PROC_TYPE_DAEMON_INTERACTIVE    0x00000400
#define CS_LAUNCH_TYPE_SYSTEM_SERVICE 1
int posix_spawnattr_setprocesstype_np(posix_spawnattr_t *, const int);
int posix_spawnattr_set_launch_type_np(posix_spawnattr_t *attr, int launch_type);
int posix_spawnattr_set_darwin_role_np(const posix_spawnattr_t * __restrict, uint64_t);

static int spawn_and_wait(const char *path, char *const argv[]) {
    pid_t pid;
    posix_spawnattr_t spattr;
    posix_spawnattr_init(&spattr);
    posix_spawnattr_setprocesstype_np(&spattr, POSIX_SPAWN_PROC_TYPE_DAEMON_INTERACTIVE);
    posix_spawnattr_set_launch_type_np(&spattr, CS_LAUNCH_TYPE_SYSTEM_SERVICE);

    int ret = posix_spawn(&pid, path, NULL, &spattr, argv, clean_environ());
    if (ret != 0) {
        fprintf(stderr, "[wayland-mac] posix_spawn %s: %s\n", path,
                strerror(ret));
        return -1;
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[wayland-mac] waitpid %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int spawn_background(const char *path, char *const argv[]) {
    pid_t pid;
    posix_spawnattr_t spattr;
    posix_spawnattr_init(&spattr);
    posix_spawnattr_setprocesstype_np(&spattr, POSIX_SPAWN_PROC_TYPE_DAEMON_INTERACTIVE);
    posix_spawnattr_set_launch_type_np(&spattr, CS_LAUNCH_TYPE_SYSTEM_SERVICE);

    if (strstr(path, "framebufferd") != NULL) {
        posix_spawnattr_set_darwin_role_np(
            &spattr,
            0x4); // PRIO_DARWIN_ROLE_UI_FOCAL
    }

    int ret = posix_spawn(&pid, path, NULL, &spattr, argv, clean_environ());
    if (ret != 0) {
        fprintf(stderr, "[wayland-mac] posix_spawn %s: %s\n", path,
                strerror(ret));
        return -1;
    }
    return 0;
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
    if (getuid() != 0) {
        fprintf(stderr, "[wayland-mac] must run as root\n");
        abort();
        return;
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
     * epoll shim.  Future DRM hooks go here too. */
    install_epoll_hooks();
    install_drm_hooks();

    /* If framebufferd's Mach service is already registered, everything is
     * already set up — skip support dir, amfi, and framebufferd spawn. */
    {
        mach_port_t port = MACH_PORT_NULL;
        kern_return_t kr = bootstrap_look_up(bootstrap_port,
                                            "com.wayland-mac.framebufferd",
                                            &port);
        if (kr == KERN_SUCCESS) {
            mach_port_deallocate(mach_task_self(), port);
            return;  /* already running, nothing to do */
        }
    }

    /* Create support directory */
    if (mkdir(SUPPORT_DIR, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "[wayland-mac] mkdir %s: %s\n", SUPPORT_DIR,
                strerror(errno));
        return;
    }

    const char *amfiexceptiond_path = SUPPORT_DIR "/amfiexceptiond";
    const char *framebufferd_path    = SUPPORT_DIR "/framebufferd";

    /* Extract and launch amfiexceptiond — wait for it to finish patching AMFI */
    if (extract_section("__DATA_OBJ", "amfiexceptiond", amfiexceptiond_path) == 0) {
        char *const argv[] = {
            (char *)amfiexceptiond_path,
            NULL
        };
        spawn_and_wait(amfiexceptiond_path, argv);
    }

    /* Extract and launch framebufferd, then wait for its Mach service */
    if (extract_section("__DATA_OBJ", "framebufferd", framebufferd_path) == 0) {
        char *const argv[] = {
            (char *)framebufferd_path,
            NULL
        };
        spawn_background(framebufferd_path, argv);
    }


    /* Extract and launch inputd (input event daemon) */
    const char *inputd_path = SUPPORT_DIR "/inputd";
    if (extract_section("__DATA_OBJ", "inputd", inputd_path) == 0) {
        char *const argv[] = {
            (char *)inputd_path,
            NULL
        };
        spawn_background(inputd_path, argv);
    }

    /* Prevent display sleep while weston is running */
    {
        char *const argv[] = {
            (char *)"/usr/bin/caffeinate",
            (char *)"-d",
            NULL
        };
        spawn_background("/usr/bin/caffeinate", argv);
    }

    {
        mach_port_t port = MACH_PORT_NULL;
        kern_return_t kr;
        do {
            kr = bootstrap_look_up(bootstrap_port,
                                   "com.wayland-mac.inputd", &port);
            if (kr != KERN_SUCCESS)
                usleep(5000);
        } while (kr != KERN_SUCCESS);
        mach_port_deallocate(mach_task_self(), port);
    }

    {
        mach_port_t port = MACH_PORT_NULL;
        kern_return_t kr;
        do {
            kr = bootstrap_look_up(bootstrap_port,
                                   "com.wayland-mac.framebufferd", &port);
            if (kr != KERN_SUCCESS)
                usleep(5000);
        } while (kr != KERN_SUCCESS);
        mach_port_deallocate(mach_task_self(), port);
    }
}

__attribute__((visibility("default")))
void wayland_mac_init(void) {
    (void)0;
}
