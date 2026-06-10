#include <_abort.h>
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

static int spawn_and_wait(const char *path, char *const argv[]) {
    pid_t pid;
    int ret = posix_spawn(&pid, path, NULL, NULL, argv, environ);
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
    int ret = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (ret != 0) {
        fprintf(stderr, "[wayland-mac] posix_spawn %s: %s\n", path,
                strerror(ret));
        return -1;
    }
    return 0;
}

static void install_drm_hooks(void)
{
    (void)0; /* TODO: add Dobby hooks for drm* functions */
}

__attribute__((constructor))
static void wayland_mac_load(void) {
    if (getuid() != 0) {
        fprintf(stderr, "[wayland-mac] must run as root\n");
        abort();
        return;
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
