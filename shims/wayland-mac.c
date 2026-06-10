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

#define SUPPORT_DIR "/tmp/libwayland-support"

extern char **environ;

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

__attribute__((constructor))
static void wayland_mac_load(void) {
    if (getuid() != 0) {
        fprintf(stderr, "[wayland-mac] must run as root\n");
        return;
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

    /* Extract and launch framebufferd */
    if (extract_section("__DATA_OBJ", "framebufferd", framebufferd_path) == 0) {
        char *const argv[] = {
            (char *)framebufferd_path,
            NULL
        };
        spawn_background(framebufferd_path, argv);
    }

    /* Block until framebufferd has registered its Mach service */
    {
        mach_port_t port = MACH_PORT_NULL;
        kern_return_t kr;
        do {
            kr = bootstrap_look_up(bootstrap_port,
                                   "com.wayland-mac.framebufferd", &port);
            if (kr != KERN_SUCCESS)
                usleep(5000); /* 5 ms */
        } while (kr != KERN_SUCCESS);
        mach_port_deallocate(mach_task_self(), port);
    }
}

__attribute__((visibility("default")))
void wayland_mac_init(void) {
    (void)0;
}
