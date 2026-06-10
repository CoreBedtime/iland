#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WESTON_DIR="$SCRIPT_DIR/weston"
BUILD_DIR="$SCRIPT_DIR/build-weston"
SHIM_BUILD="$SCRIPT_DIR/build"
DEPS_DIR="$BUILD_DIR/deps"
STUB_INCDIR="$DEPS_DIR/include"
STUB_LIBDIR="$DEPS_DIR/lib"
STUB_PCDIR="$DEPS_DIR/pkgconfig"
SHIM_DYLIB="$SHIM_BUILD/libwayland-mac.dylib"

# ============================================================
# 1. Build the shim
# ============================================================
echo "=== Building shim ==="
if [ ! -f "$SHIM_DYLIB" ]; then
    (cd "$SCRIPT_DIR" && ./compile.sh)
fi
echo "Shim: $SHIM_DYLIB"

# ============================================================
# 2. Create stub headers + stub libraries for Linux-only deps
# ============================================================
# Clean build dir before creating deps (so --wipe doesn't nuke them)
rm -rf "$BUILD_DIR"
echo "=== Creating stubs ==="
mkdir -p "$STUB_INCDIR/libevdev" "$STUB_INCDIR/linux" "$STUB_LIBDIR" "$STUB_PCDIR"

cat > "$STUB_INCDIR/linux/types.h" << 'EOF'
typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef signed char __s8;
typedef short __s16;
typedef int __s32;
typedef long long __s64;
EOF

cat > "$STUB_INCDIR/pty.h" << 'EOF'
/* macOS: pty.h maps to util.h */
#include <util.h>
EOF

cat > "$STUB_INCDIR/malloc.h" << 'EOF'
#include <stdlib.h>
EOF

cat > "$STUB_INCDIR/drm.h" << 'EOF'
#define DRM_MODE_CONNECTOR_LVDS 7
#define DRM_MODE_CONNECTOR_eDP  14
EOF

cat > "$STUB_INCDIR/linux/limits.h" << 'EOF'
#ifndef _LINUX_LIMITS_H
#define _LINUX_LIMITS_H
/* linux/limits.h maps to <sys/syslimits.h> on macOS. */
#include <sys/syslimits.h>
#endif
EOF

cat > "$STUB_INCDIR/endian.h" << 'EOF'
/* Stub <endian.h> using macOS <machine/endian.h>. */
#include <machine/endian.h>
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN 4321
#endif
#ifndef __BYTE_ORDER
#if defined(__LITTLE_ENDIAN__) || defined(__arm64__) || defined(__i386__) || defined(__x86_64__)
#define __BYTE_ORDER __LITTLE_ENDIAN
#else
#define __BYTE_ORDER __BIG_ENDIAN
#endif
#endif
EOF

cat > "$STUB_INCDIR/values.h" << 'EOF'
/* System V values.h — constants for machine-dependent values.
 * On modern systems these come from <limits.h> and <float.h>. */
#include <limits.h>
#include <float.h>
#ifndef MAXINT
#define MAXINT  INT_MAX
#endif
#ifndef MAXLONG
#define MAXLONG LONG_MAX
#endif
#ifndef MAXFLOAT
#define MAXFLOAT FLT_MAX
#endif
#ifndef MAXDOUBLE
#define MAXDOUBLE DBL_MAX
#endif
EOF

cat > "$STUB_INCDIR/linux/types.h" << 'EOF'
#ifndef _LINUX_TYPES_H
#define _LINUX_TYPES_H
/* Minimal linux/types.h stub for weston. */
#include <stdint.h>
typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;
#endif
EOF

cat > "$STUB_INCDIR/linux/ioctl.h" << 'EOF'
#ifndef _LINUX_IOCTL_H
#define _LINUX_IOCTL_H
/* Minimal linux/ioctl.h stub for weston. Provides _IOWR needed by
 * linux-sync-file-uapi.h and the _IO/_IOR/_IOW/_IOWR macros.
 * On macOS these come from <sys/ioccom.h>; only define if missing. */
#include <linux/types.h>
#include <sys/ioccom.h>

#ifndef _IOC_NONE
#define _IOC_NONE  0U
#endif
#ifndef _IOC_WRITE
#define _IOC_WRITE 1U
#endif
#ifndef _IOC_READ
#define _IOC_READ  2U
#endif

#ifndef _IOC
#define _IOC(inout, group, nr, len) \
    (((inout) << 30) | ((nr) << 0) | ((group) << 8) | ((len) << 16))
#endif

#ifndef _IO
#define _IO(type, nr)        _IOC(_IOC_NONE, (type), (nr), 0)
#endif
#ifndef _IOR
#define _IOR(type, nr, size) _IOC(_IOC_READ,  (type), (nr), sizeof(size))
#endif
#ifndef _IOW
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#endif
#ifndef _IOWR
#define _IOWR(type, nr, size) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))
#endif
#endif
EOF

cat > "$STUB_INCDIR/linux/dma-buf.h" << 'EOF'
#ifndef _LINUX_DMA_BUF_H
#define _LINUX_DMA_BUF_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define DMA_BUF_SYNC_READ      (1 << 0)
#define DMA_BUF_SYNC_WRITE     (1 << 1)
#define DMA_BUF_SYNC_START     (0 << 2)
#define DMA_BUF_SYNC_END       (1 << 2)

struct dma_buf_sync {
	__u64 flags;
};

#define DMA_BUF_IOCTL_SYNC     _IOW('b', 0, struct dma_buf_sync)
#endif
EOF

cat > "$STUB_INCDIR/linux/udmabuf.h" << 'EOF'
#ifndef _LINUX_UDMABUF_H
#define _LINUX_UDMABUF_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define UDMABUF_PATH "/dev/udmabuf"

struct udmabuf_create_item {
	__u32 memfd;
	__u32 __pad;
	__u64 offset;
	__u64 size;
};

struct udmabuf_create {
	__u32 memfd;
	__u32 flags;
	__u64 offset;
	__u64 size;
};

struct udmabuf_create_list {
	__u32 flags;
	__u32 count;
	struct udmabuf_create_item list[];
};

#define UDMABUF_FLAGS_CLOEXEC  0x01
#define UDMABUF_CREATE       _IOW('u', 0x42, struct udmabuf_create)
#define UDMABUF_CREATE_LIST  _IOW('u', 0x43, struct udmabuf_create_list)
#endif
EOF

cat > "$STUB_INCDIR/linux/vt.h" << 'EOF'
/* stubs for macos */
#include <sys/ioctl.h>
EOF

cat > "$STUB_INCDIR/linux/input.h" << 'EOF'
#ifndef _LINUX_INPUT_H
#define _LINUX_INPUT_H
/* Minimal stub: only what weston actually uses (55+ files include this).
 * Real linux/input.h is ~2500 lines; these 30 constants suffice. */
#define EV_KEY          0x01

#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_BACKSPACE   14
#define KEY_TAB         15
#define KEY_C           46
#define KEY_D           32
#define KEY_F           33
#define KEY_H           35
#define KEY_K           37
#define KEY_M           50
#define KEY_O           24
#define KEY_R           19
#define KEY_S           31
#define KEY_V           47
#define KEY_F1          59
#define KEY_F2          60
#define KEY_F3          61
#define KEY_F4          62
#define KEY_F5          63
#define KEY_F6          64
#define KEY_F7          65
#define KEY_F8          66
#define KEY_F9          67
#define KEY_F10         68
#define KEY_F11         87
#define KEY_SPACE       57
#define KEY_LEFTSHIFT   42
#define KEY_UP          103
#define KEY_DOWN        108
#define KEY_LEFT        105
#define KEY_RIGHT       106
#define KEY_BRIGHTNESSDOWN 224
#define KEY_BRIGHTNESSUP   225

#define BTN_LEFT        0x110
#define BTN_RIGHT       0x111
#define BTN_MIDDLE      0x112
#define BTN_SIDE        0x113
#define BTN_EXTRA       0x114
#define BTN_TOUCH       0x14a
#endif
EOF

# ---- macOS compat stubs for Linux-only symbols that meson detects via has_function() ---- 
cat > "$STUB_INCDIR/_macos_stubs.h" << 'EOF'
#ifndef _MACOS_STUBS_H
#define _MACOS_STUBS_H
/* Provide implementations for functions meson detected via has_function()
 * but that don't actually exist on macOS (because b_lundef=false fools the check). */
#include <sys/fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/ioctl.h>

/* macOS howmany uses modulo which fails with double operands; override */
#undef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))

/* funopen may be hidden by _POSIX_C_SOURCE; declare it manually */
FILE *funopen(const void *cookie,
              int (*readfn)(void *, char *, int),
              int (*writefn)(void *, const char *, int),
              fpos_t (*seekfn)(void *, fpos_t, int),
              int (*closefn)(void *));

/* fopencookie (GNU) -> funopen (BSD/macOS) wrapper */
typedef struct {
    ssize_t (*read)(void *, char *, size_t);
    ssize_t (*write)(void *, const char *, size_t);
    int (*seek)(void *, fpos_t *, int);
    int (*close)(void *);
} cookie_io_functions_t;

static inline int _fopencookie_write(void *cookie, const char *buf, int size) {
    cookie_io_functions_t *f = (cookie_io_functions_t *)(((void **)cookie) + 1);
    return (int)f->write(cookie, buf, (size_t)size);
}
static inline int _fopencookie_close(void *cookie) {
    cookie_io_functions_t *f = (cookie_io_functions_t *)(((void **)cookie) + 1);
    return f->close(cookie);
}
static inline FILE *fopencookie(void *cookie, const char *mode,
                                 cookie_io_functions_t io_funcs) {
    (void)mode;
    /* funopen on macOS: read/write return int, take int size.
     * We pack the cookie + io_funcs into a simple struct. */
    void **wrapper = malloc(2 * sizeof(void *));
    if (!wrapper) return NULL;
    wrapper[0] = cookie;
    memcpy(wrapper + 1, &io_funcs, sizeof(io_funcs));
    return funopen(wrapper, NULL, _fopencookie_write, NULL, _fopencookie_close);
}
/* struct itimerspec is Linux-only; macOS has <time.h> with struct timespec */
#ifndef _STRUCT_ITIMERSPEC
#define _STRUCT_ITIMERSPEC
struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};
#endif

/* pipe2 is Linux-only; emulate via pipe() + fcntl */
static inline int pipe2(int pipefd[2], int flags) {
    int ret = pipe(pipefd);
    if (ret == -1) return -1;
    if (flags & O_CLOEXEC) {
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    }
    if (flags & O_NONBLOCK) {
        int fl0 = fcntl(pipefd[0], F_GETFL);
        int fl1 = fcntl(pipefd[1], F_GETFL);
        fcntl(pipefd[0], F_SETFL, fl0 | O_NONBLOCK);
        fcntl(pipefd[1], F_SETFL, fl1 | O_NONBLOCK);
    }
    return 0;
}

/* Linux-specific coarse monotonic clock; macOS has no equivalent */
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0001
#endif
#ifndef F_SEAL_GROW
#define F_SEAL_GROW 0x0002
#endif
#ifndef F_SEAL_WRITE
#define F_SEAL_WRITE 0x0004
#endif

static inline int memfd_create(const char *name, unsigned int flags) {
    (void)name; (void)flags;
    errno = ENOSYS;
    return -1;
}
static inline int posix_fallocate(int fd, off_t offset, off_t len) {
    (void)fd; (void)offset; (void)len;
    /* macOS: fstore_t + fcntl(F_PREALLOCATE) or just ftruncate */
    errno = ENOSYS;
    return -1;
}
#endif
EOF
# Add forced include of the stubs header
FORCE_INCLUDE="-include ${STUB_INCDIR}/_macos_stubs.h"

# ---- libevdev stub ----
cat > "$STUB_INCDIR/libevdev/libevdev.h" << 'EOF'
int libevdev_event_code_from_name(unsigned int type, const char *name);
EOF

cat > "$DEPS_DIR/libevdev-stub.c" << 'EOF'
int libevdev_event_code_from_name(const char *name, int len) { return -1; }
EOF
cc -dynamiclib -o "$STUB_LIBDIR/libevdev.dylib" "$DEPS_DIR/libevdev-stub.c" -install_name "$STUB_LIBDIR/libevdev.dylib"

# ---- libudev stub ----
cat > "$STUB_INCDIR/libudev.h" << 'EOF'
#include <sys/types.h>
struct udev;
struct udev_device;
struct udev_enumerate;
struct udev_list_entry;
struct udev_monitor;
struct udev *udev_new(void);
struct udev *udev_unref(struct udev *);
struct udev_device *udev_device_new_from_syspath(struct udev *, const char *);
struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *, const char *, const char *);
struct udev_device *udev_device_ref(struct udev_device *);
struct udev_device *udev_device_unref(struct udev_device *);
struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *, const char *, const char *);
const char *udev_device_get_syspath(struct udev_device *);
const char *udev_device_get_sysattr_value(struct udev_device *, const char *);
const char *udev_device_get_property_value(struct udev_device *, const char *);
const char *udev_device_get_devnode(struct udev_device *);
const char *udev_device_get_sysnum(struct udev_device *);
dev_t udev_device_get_devnum(struct udev_device *);
struct udev_enumerate *udev_enumerate_new(struct udev *);
int udev_enumerate_add_match_subsystem(struct udev_enumerate *, const char *);
int udev_enumerate_add_match_sysname(struct udev_enumerate *, const char *);
int udev_enumerate_scan_devices(struct udev_enumerate *);
struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *);
struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *);
struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *);
const char *udev_list_entry_get_name(struct udev_list_entry *);
#define udev_list_entry_foreach(e, list) for (e = list; e; e = udev_list_entry_get_next(e))
struct udev_monitor *udev_monitor_new_from_netlink(struct udev *, const char *);
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *, const char *, const char *);
int udev_monitor_enable_receiving(struct udev_monitor *);
int udev_monitor_get_fd(struct udev_monitor *);
struct udev_device *udev_monitor_receive_device(struct udev_monitor *);
struct udev_monitor *udev_monitor_unref(struct udev_monitor *);
EOF

cat > "$DEPS_DIR/libudev-stub.c" << 'EOF'
#include <stddef.h>
#include <sys/types.h>
struct udev *udev_new(void) { return NULL; }
struct udev *udev_unref(struct udev *u) { return NULL; }
struct udev_device *udev_device_new_from_syspath(struct udev *u, const char *p) { return NULL; }
struct udev_device *udev_device_new_from_subsystem_sysname(struct udev *u, const char *a, const char *b) { return NULL; }
struct udev_device *udev_device_ref(struct udev_device *d) { return NULL; }
struct udev_device *udev_device_unref(struct udev_device *d) { return NULL; }
struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *d, const char *a, const char *b) { return NULL; }
const char *udev_device_get_syspath(struct udev_device *d) { return NULL; }
const char *udev_device_get_sysattr_value(struct udev_device *d, const char *a) { return NULL; }
const char *udev_device_get_property_value(struct udev_device *d, const char *k) { return NULL; }
const char *udev_device_get_devnode(struct udev_device *d) { return NULL; }
const char *udev_device_get_sysnum(struct udev_device *d) { return NULL; }
dev_t udev_device_get_devnum(struct udev_device *d) { return 0; }
struct udev_enumerate *udev_enumerate_new(struct udev *u) { return NULL; }
int udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char *s) { return 0; }
int udev_enumerate_add_match_sysname(struct udev_enumerate *e, const char *s) { return 0; }
int udev_enumerate_scan_devices(struct udev_enumerate *e) { return 0; }
struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *e) { return NULL; }
struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *e) { return NULL; }
struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *e) { return NULL; }
const char *udev_list_entry_get_name(struct udev_list_entry *e) { return NULL; }
struct udev_monitor *udev_monitor_new_from_netlink(struct udev *u, const char *n) { return NULL; }
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *m, const char *a, const char *b) { return 0; }
int udev_monitor_enable_receiving(struct udev_monitor *m) { return 0; }
int udev_monitor_get_fd(struct udev_monitor *m) { return -1; }
struct udev_device *udev_monitor_receive_device(struct udev_monitor *m) { return NULL; }
struct udev_monitor *udev_monitor_unref(struct udev_monitor *m) { return NULL; }
EOF
cc -dynamiclib -o "$STUB_LIBDIR/libudev.dylib" "$DEPS_DIR/libudev-stub.c" -install_name "$STUB_LIBDIR/libudev.dylib"

# ---- libinput stub ----
cat > "$STUB_INCDIR/libinput.h" << 'EOF'
#ifndef LIBINPUT_H
#define LIBINPUT_H
#include <stdint.h>
#include <stddef.h>
enum libinput_event_type {
    LIBINPUT_EVENT_NONE=0, LIBINPUT_EVENT_DEVICE_ADDED, LIBINPUT_EVENT_DEVICE_REMOVED,
    LIBINPUT_EVENT_KEYBOARD_KEY, LIBINPUT_EVENT_POINTER_MOTION,
    LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE, LIBINPUT_EVENT_POINTER_BUTTON,
    LIBINPUT_EVENT_POINTER_AXIS, LIBINPUT_EVENT_TOUCH_DOWN, LIBINPUT_EVENT_TOUCH_UP,
    LIBINPUT_EVENT_TOUCH_MOTION, LIBINPUT_EVENT_TOUCH_CANCEL,
    LIBINPUT_EVENT_TABLET_TOOL_AXIS, LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY,
    LIBINPUT_EVENT_TABLET_TOOL_TIP, LIBINPUT_EVENT_TABLET_TOOL_BUTTON,
    LIBINPUT_EVENT_TABLET_PAD_BUTTON, LIBINPUT_EVENT_TABLET_PAD_RING,
    LIBINPUT_EVENT_TABLET_PAD_STRIP, LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN,
    LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE, LIBINPUT_EVENT_GESTURE_SWIPE_END,
    LIBINPUT_EVENT_GESTURE_PINCH_BEGIN, LIBINPUT_EVENT_GESTURE_PINCH_UPDATE,
    LIBINPUT_EVENT_GESTURE_PINCH_END, LIBINPUT_EVENT_SWITCH_TOGGLE,
};
enum libinput_capability { LIBINPUT_DEVICE_CAP_KEYBOARD=0, LIBINPUT_DEVICE_CAP_POINTER=1,
    LIBINPUT_DEVICE_CAP_TOUCH=2, LIBINPUT_DEVICE_CAP_TABLET_TOOL=3,
    LIBINPUT_DEVICE_CAP_TABLET_PAD=4, LIBINPUT_DEVICE_CAP_GESTURE=5, LIBINPUT_DEVICE_CAP_SWITCH=6 };
enum libinput_led { LIBINPUT_LED_NUM_LOCK=1, LIBINPUT_LED_CAPS_LOCK=2, LIBINPUT_LED_SCROLL_LOCK=4 };
enum libinput_pointer_axis { LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL=0, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL=1 };
enum libinput_pointer_axis_source { LIBINPUT_POINTER_AXIS_SOURCE_WHEEL=1,
    LIBINPUT_POINTER_AXIS_SOURCE_FINGER=2, LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS=3,
    LIBINPUT_POINTER_AXIS_SOURCE_WHEEL_TILT=4 };
enum libinput_config_accel_profile { LIBINPUT_CONFIG_ACCEL_PROFILE_NONE=0,
    LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT=1, LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE=2 };
enum libinput_config_scroll_method { LIBINPUT_CONFIG_SCROLL_NO_SCROLL=0,
    LIBINPUT_CONFIG_SCROLL_2FG=1, LIBINPUT_CONFIG_SCROLL_EDGE=2, LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN=3 };
enum libinput_config_status { LIBINPUT_CONFIG_STATUS_SUCCESS=0,
    LIBINPUT_CONFIG_STATUS_UNSUPPORTED=1, LIBINPUT_CONFIG_STATUS_INVALID=2 };
enum libinput_tablet_tool_type { LIBINPUT_TABLET_TOOL_TYPE_PEN=1, LIBINPUT_TABLET_TOOL_TYPE_ERASER=2 };
struct libinput;
struct libinput_event;
struct libinput_device;
struct libinput_seat;
struct libinput_tablet_tool;
struct libinput_interface { int (*open_restricted)(const char *, int, void *); void (*close_restricted)(int, void *); };

struct libinput *libinput_udev_create_context(const struct libinput_interface *, void *, struct udev *);
struct libinput *libinput_unref(struct libinput *);
int libinput_udev_assign_seat(struct libinput *, const char *);
int libinput_get_fd(struct libinput *);
int libinput_dispatch(struct libinput *);
struct libinput_event *libinput_get_event(struct libinput *);
void libinput_event_destroy(struct libinput_event *);
struct libinput *libinput_event_get_context(struct libinput_event *);
enum libinput_event_type libinput_event_get_type(struct libinput_event *);
struct libinput_device *libinput_event_get_device(struct libinput_event *);
int libinput_suspend(struct libinput *);
int libinput_resume(struct libinput *);
struct libinput_device *libinput_device_ref(struct libinput_device *);
struct libinput_device *libinput_device_unref(struct libinput_device *);
void libinput_device_set_user_data(struct libinput_device *, void *);
void *libinput_device_get_user_data(struct libinput_device *);
int libinput_device_has_capability(struct libinput_device *, enum libinput_capability);
const char *libinput_device_get_name(struct libinput_device *);
const char *libinput_device_get_sysname(struct libinput_device *);
const char *libinput_device_get_output_name(struct libinput_device *);
unsigned int libinput_device_get_id_product(struct libinput_device *);
unsigned int libinput_device_get_id_vendor(struct libinput_device *);
struct libinput_seat *libinput_device_get_seat(struct libinput_device *);
struct udev_device *libinput_device_get_udev_device(struct libinput_device *);
int libinput_device_config_tap_set_enabled(struct libinput_device *, int);
int libinput_device_config_tap_get_finger_count(struct libinput_device *);
int libinput_device_config_tap_set_drag_enabled(struct libinput_device *, int);
int libinput_device_config_tap_set_drag_lock_enabled(struct libinput_device *, int);
int libinput_device_config_calibration_has_matrix(struct libinput_device *);
int libinput_device_config_calibration_set_matrix(struct libinput_device *, const float[9]);
int libinput_device_config_calibration_get_matrix(struct libinput_device *, float[9]);
int libinput_device_config_calibration_get_default_matrix(struct libinput_device *, float[9]);
int libinput_device_config_left_handed_is_available(struct libinput_device *);
int libinput_device_config_left_handed_set(struct libinput_device *, int);
int libinput_device_config_middle_emulation_is_available(struct libinput_device *);
int libinput_device_config_middle_emulation_set_enabled(struct libinput_device *, int);
int libinput_device_config_dwt_is_available(struct libinput_device *);
int libinput_device_config_dwt_set_enabled(struct libinput_device *, int);
int libinput_device_config_rotation_is_available(struct libinput_device *);
int libinput_device_config_rotation_set_angle(struct libinput_device *, unsigned int);
int libinput_device_config_accel_is_available(struct libinput_device *);
enum libinput_config_status libinput_device_config_accel_set_speed(struct libinput_device *, double);
enum libinput_config_accel_profile libinput_device_config_accel_get_profiles(struct libinput_device *);
enum libinput_config_status libinput_device_config_accel_set_profile(struct libinput_device *, enum libinput_config_accel_profile);
enum libinput_config_scroll_method libinput_device_config_scroll_get_methods(struct libinput_device *);
enum libinput_config_status libinput_device_config_scroll_set_method(struct libinput_device *, enum libinput_config_scroll_method);
int libinput_device_config_scroll_set_button(struct libinput_device *, uint32_t);
int libinput_device_config_scroll_has_natural_scroll(struct libinput_device *);
int libinput_device_config_scroll_set_natural_scroll_enabled(struct libinput_device *, int);
int libinput_device_led_update(struct libinput_device *, enum libinput_led);
const char *libinput_seat_get_logical_name(struct libinput_seat *);
struct libinput_event_keyboard *libinput_event_get_keyboard_event(struct libinput_event *);
uint32_t libinput_event_keyboard_get_key(struct libinput_event_keyboard *);
int libinput_event_keyboard_get_key_state(struct libinput_event_keyboard *);
uint32_t libinput_event_keyboard_get_seat_key_count(struct libinput_event_keyboard *);
uint64_t libinput_event_keyboard_get_time_usec(struct libinput_event_keyboard *);
struct libinput_event_pointer *libinput_event_get_pointer_event(struct libinput_event *);
double libinput_event_pointer_get_dx(struct libinput_event_pointer *);
double libinput_event_pointer_get_dy(struct libinput_event_pointer *);
double libinput_event_pointer_get_dx_unaccelerated(struct libinput_event_pointer *);
double libinput_event_pointer_get_dy_unaccelerated(struct libinput_event_pointer *);
double libinput_event_pointer_get_absolute_x_transformed(struct libinput_event_pointer *, uint32_t);
double libinput_event_pointer_get_absolute_y_transformed(struct libinput_event_pointer *, uint32_t);
uint32_t libinput_event_pointer_get_button(struct libinput_event_pointer *);
int libinput_event_pointer_get_button_state(struct libinput_event_pointer *);
uint32_t libinput_event_pointer_get_seat_button_count(struct libinput_event_pointer *);
uint64_t libinput_event_pointer_get_time_usec(struct libinput_event_pointer *);
int libinput_event_pointer_has_axis(struct libinput_event_pointer *, enum libinput_pointer_axis);
double libinput_event_pointer_get_axis_value(struct libinput_event_pointer *, enum libinput_pointer_axis);
double libinput_event_pointer_get_axis_value_discrete(struct libinput_event_pointer *, enum libinput_pointer_axis);
enum libinput_pointer_axis_source libinput_event_pointer_get_axis_source(struct libinput_event_pointer *);
struct libinput_event_touch *libinput_event_get_touch_event(struct libinput_event *);
int32_t libinput_event_touch_get_seat_slot(struct libinput_event_touch *);
uint64_t libinput_event_touch_get_time_usec(struct libinput_event_touch *);
double libinput_event_touch_get_x_transformed(struct libinput_event_touch *, uint32_t);
double libinput_event_touch_get_y_transformed(struct libinput_event_touch *, uint32_t);
struct libinput_event_tablet_tool *libinput_event_get_tablet_tool_event(struct libinput_event *);
double libinput_event_tablet_tool_get_x_transformed(struct libinput_event_tablet_tool *, uint32_t);
double libinput_event_tablet_tool_get_y_transformed(struct libinput_event_tablet_tool *, uint32_t);
double libinput_event_tablet_tool_get_pressure(struct libinput_event_tablet_tool *);
double libinput_event_tablet_tool_get_distance(struct libinput_event_tablet_tool *);
double libinput_event_tablet_tool_get_tilt_x(struct libinput_event_tablet_tool *);
double libinput_event_tablet_tool_get_tilt_y(struct libinput_event_tablet_tool *);
int libinput_event_tablet_tool_get_tip_state(struct libinput_event_tablet_tool *);
int libinput_event_tablet_tool_get_proximity_state(struct libinput_event_tablet_tool *);
uint32_t libinput_event_tablet_tool_get_button(struct libinput_event_tablet_tool *);
uint32_t libinput_event_tablet_tool_get_button_state(struct libinput_event_tablet_tool *);
uint64_t libinput_event_tablet_tool_get_time(struct libinput_event_tablet_tool *);
int libinput_event_tablet_tool_x_has_changed(struct libinput_event_tablet_tool *);
int libinput_event_tablet_tool_y_has_changed(struct libinput_event_tablet_tool *);
int libinput_event_tablet_tool_pressure_has_changed(struct libinput_event_tablet_tool *);
int libinput_event_tablet_tool_distance_has_changed(struct libinput_event_tablet_tool *);
int libinput_event_tablet_tool_tilt_x_has_changed(struct libinput_event_tablet_tool *);
int libinput_event_tablet_tool_tilt_y_has_changed(struct libinput_event_tablet_tool *);
struct libinput_tablet_tool *libinput_event_tablet_tool_get_tool(struct libinput_event_tablet_tool *);
enum libinput_tablet_tool_type libinput_tablet_tool_get_type(struct libinput_tablet_tool *);
uint64_t libinput_tablet_tool_get_tool_id(struct libinput_tablet_tool *);
uint64_t libinput_tablet_tool_get_serial(struct libinput_tablet_tool *);
int libinput_tablet_tool_is_unique(struct libinput_tablet_tool *);
int libinput_tablet_tool_has_pressure(struct libinput_tablet_tool *);
int libinput_tablet_tool_has_distance(struct libinput_tablet_tool *);
int libinput_tablet_tool_has_tilt(struct libinput_tablet_tool *);
void libinput_tablet_tool_set_user_data(struct libinput_tablet_tool *, void *);
void *libinput_tablet_tool_get_user_data(struct libinput_tablet_tool *);
void *libinput_get_user_data(struct libinput *);
enum libinput_log_priority { LIBINPUT_LOG_PRIORITY_INFO=4, LIBINPUT_LOG_PRIORITY_DEBUG=7 };
void libinput_log_set_handler(void (*)(struct libinput *, enum libinput_log_priority, const char *, void *));
#endif /* LIBINPUT_H */
EOF

# Generate stub implementations from the header declarations
sed -n 's/^\([a-zA-Z_][a-zA-Z_0-9]* \?[*]*\) \([a-zA-Z_][a-zA-Z_0-9]*\)(.*);$/IMPLEMENT(\1,\2)/p' "$STUB_INCDIR/libinput.h" | while read line; do
    :
done

# Manual stub impl for libinput
cat > "$DEPS_DIR/libinput-stub.c" << 'STUBEOF'
#include <stddef.h>
#include <stdint.h>
int libinput_udev_create_context(const void *i, void *u, void *ud) { (void)i;(void)u;(void)ud; return 0; }
void *libinput_unref(void *l) { (void)l; return NULL; }
int libinput_udev_assign_seat(void *l, const char *s) { (void)l;(void)s; return -1; }
int libinput_get_fd(void *l) { (void)l; return -1; }
int libinput_dispatch(void *l) { (void)l; return 0; }
void *libinput_get_event(void *l) { (void)l; return NULL; }
void libinput_event_destroy(void *e) { (void)e; }
void *libinput_event_get_context(void *e) { (void)e; return NULL; }
int libinput_event_get_type(void *e) { (void)e; return 0; }
void *libinput_event_get_device(void *e) { (void)e; return NULL; }
int libinput_suspend(void *l) { (void)l; return 0; }
int libinput_resume(void *l) { (void)l; return 0; }
void *libinput_device_ref(void *d) { (void)d; return NULL; }
void *libinput_device_unref(void *d) { (void)d; return NULL; }
void libinput_device_set_user_data(void *d, void *p) { (void)d;(void)p; }
void *libinput_device_get_user_data(void *d) { (void)d; return NULL; }
int libinput_device_has_capability(void *d, int c) { (void)d;(void)c; return 0; }
const char *libinput_device_get_name(void *d) { (void)d; return "stub"; }
const char *libinput_device_get_sysname(void *d) { (void)d; return "stub"; }
const char *libinput_device_get_output_name(void *d) { (void)d; return NULL; }
unsigned int libinput_device_get_id_product(void *d) { (void)d; return 0; }
unsigned int libinput_device_get_id_vendor(void *d) { (void)d; return 0; }
void *libinput_device_get_seat(void *d) { (void)d; return NULL; }
void *libinput_device_get_udev_device(void *d) { (void)d; return NULL; }
int libinput_device_config_tap_set_enabled(void *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_tap_get_finger_count(void *d) { (void)d; return 0; }
int libinput_device_config_tap_set_drag_enabled(void *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_tap_set_drag_lock_enabled(void *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_calibration_has_matrix(void *d) { (void)d; return 0; }
int libinput_device_config_calibration_set_matrix(void *d, const float m[9]) { (void)d;(void)m; return 0; }
int libinput_device_config_calibration_get_matrix(void *d, float m[9]) { (void)d;(void)m; return 0; }
void libinput_device_config_calibration_get_default_matrix(void *d, float m[9]) { (void)d;(void)m; }
int libinput_device_config_left_handed_is_available(void *d) { (void)d; return 0; }
int libinput_device_config_left_handed_set(void *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_middle_emulation_is_available(void *d) { (void)d; return 0; }
int libinput_device_config_middle_emulation_set_enabled(void *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_dwt_is_available(void *d) { (void)d; return 0; }
int libinput_device_config_dwt_set_enabled(void *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_rotation_is_available(void *d) { (void)d; return 0; }
int libinput_device_config_rotation_set_angle(void *d, unsigned int a) { (void)d;(void)a; return 0; }
int libinput_device_config_accel_is_available(void *d) { (void)d; return 0; }
int libinput_device_config_accel_set_speed(void *d, double s) { (void)d;(void)s; return 1; }
int libinput_device_config_accel_get_profiles(void *d) { (void)d; return 0; }
int libinput_device_config_accel_set_profile(void *d, int p) { (void)d;(void)p; return 1; }
int libinput_device_config_scroll_get_methods(void *d) { (void)d; return 0; }
int libinput_device_config_scroll_set_method(void *d, int m) { (void)d;(void)m; return 1; }
int libinput_device_config_scroll_set_button(void *d, uint32_t b) { (void)d;(void)b; return 0; }
int libinput_device_config_scroll_has_natural_scroll(void *d) { (void)d; return 0; }
int libinput_device_config_scroll_set_natural_scroll_enabled(void *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_led_update(void *d, int l) { (void)d;(void)l; return 0; }
const char *libinput_seat_get_logical_name(void *s) { (void)s; return "default"; }
void *libinput_event_get_keyboard_event(void *e) { (void)e; return NULL; }
uint32_t libinput_event_keyboard_get_key(void *e) { (void)e; return 0; }
int libinput_event_keyboard_get_key_state(void *e) { (void)e; return 0; }
uint32_t libinput_event_keyboard_get_seat_key_count(void *e) { (void)e; return 0; }
uint64_t libinput_event_keyboard_get_time_usec(void *e) { (void)e; return 0; }
void *libinput_event_get_pointer_event(void *e) { (void)e; return NULL; }
double libinput_event_pointer_get_dx(void *e) { (void)e; return 0; }
double libinput_event_pointer_get_dy(void *e) { (void)e; return 0; }
double libinput_event_pointer_get_dx_unaccelerated(void *e) { (void)e; return 0; }
double libinput_event_pointer_get_dy_unaccelerated(void *e) { (void)e; return 0; }
double libinput_event_pointer_get_absolute_x_transformed(void *e, uint32_t w) { (void)e;(void)w; return 0; }
double libinput_event_pointer_get_absolute_y_transformed(void *e, uint32_t h) { (void)e;(void)h; return 0; }
uint32_t libinput_event_pointer_get_button(void *e) { (void)e; return 0; }
int libinput_event_pointer_get_button_state(void *e) { (void)e; return 0; }
uint32_t libinput_event_pointer_get_seat_button_count(void *e) { (void)e; return 0; }
uint64_t libinput_event_pointer_get_time_usec(void *e) { (void)e; return 0; }
int libinput_event_pointer_has_axis(void *e, int a) { (void)e;(void)a; return 0; }
double libinput_event_pointer_get_axis_value(void *e, int a) { (void)e;(void)a; return 0; }
double libinput_event_pointer_get_axis_value_discrete(void *e, int a) { (void)e;(void)a; return 0; }
int libinput_event_pointer_get_axis_source(void *e) { (void)e; return 0; }
void *libinput_event_get_touch_event(void *e) { (void)e; return NULL; }
int32_t libinput_event_touch_get_seat_slot(void *e) { (void)e; return 0; }
uint64_t libinput_event_touch_get_time_usec(void *e) { (void)e; return 0; }
double libinput_event_touch_get_x_transformed(void *e, uint32_t w) { (void)e;(void)w; return 0; }
double libinput_event_touch_get_y_transformed(void *e, uint32_t h) { (void)e;(void)h; return 0; }
void *libinput_event_get_tablet_tool_event(void *e) { (void)e; return NULL; }
double libinput_event_tablet_tool_get_x_transformed(void *e, uint32_t w) { (void)e;(void)w; return 0; }
double libinput_event_tablet_tool_get_y_transformed(void *e, uint32_t h) { (void)e;(void)h; return 0; }
double libinput_event_tablet_tool_get_pressure(void *e) { (void)e; return 0; }
double libinput_event_tablet_tool_get_distance(void *e) { (void)e; return 0; }
double libinput_event_tablet_tool_get_tilt_x(void *e) { (void)e; return 0; }
double libinput_event_tablet_tool_get_tilt_y(void *e) { (void)e; return 0; }
int libinput_event_tablet_tool_get_tip_state(void *e) { (void)e; return 0; }
int libinput_event_tablet_tool_get_proximity_state(void *e) { (void)e; return 0; }
uint32_t libinput_event_tablet_tool_get_button(void *e) { (void)e; return 0; }
uint32_t libinput_event_tablet_tool_get_button_state(void *e) { (void)e; return 0; }
uint64_t libinput_event_tablet_tool_get_time(void *e) { (void)e; return 0; }
int libinput_event_tablet_tool_x_has_changed(void *e) { (void)e; return 0; }
int libinput_event_tablet_tool_y_has_changed(void *e) { (void)e; return 0; }
int libinput_event_tablet_tool_pressure_has_changed(void *e) { (void)e; return 0; }
int libinput_event_tablet_tool_distance_has_changed(void *e) { (void)e; return 0; }
int libinput_event_tablet_tool_tilt_x_has_changed(void *e) { (void)e; return 0; }
int libinput_event_tablet_tool_tilt_y_has_changed(void *e) { (void)e; return 0; }
void *libinput_event_tablet_tool_get_tool(void *e) { (void)e; return NULL; }
int libinput_tablet_tool_get_type(void *t) { (void)t; return 0; }
uint64_t libinput_tablet_tool_get_tool_id(void *t) { (void)t; return 0; }
uint64_t libinput_tablet_tool_get_serial(void *t) { (void)t; return 0; }
int libinput_tablet_tool_is_unique(void *t) { (void)t; return 0; }
int libinput_tablet_tool_has_pressure(void *t) { (void)t; return 0; }
int libinput_tablet_tool_has_distance(void *t) { (void)t; return 0; }
int libinput_tablet_tool_has_tilt(void *t) { (void)t; return 0; }
void libinput_tablet_tool_set_user_data(void *t, void *d) { (void)t;(void)d; }
void *libinput_tablet_tool_get_user_data(void *t) { (void)t; return NULL; }
STUBEOF
cc -dynamiclib -o "$STUB_LIBDIR/libinput.dylib" "$DEPS_DIR/libinput-stub.c" \
    -install_name "$STUB_LIBDIR/libinput.dylib"

# ---- libseat stub ----
cat > "$STUB_INCDIR/libseat.h" << 'EOF'
#include <stddef.h>
#include <stdarg.h>
struct libseat { int fd; int vt; };
struct libseat_device { int dummy; };
enum libseat_log_level { LIBSEAT_LOG_LEVEL_NONE=0, LIBSEAT_LOG_LEVEL_INFO=1,
    LIBSEAT_LOG_LEVEL_DEBUG=2 };
struct libseat_seat_listener {
    void (*enable_seat)(struct libseat *, void *);
    void (*disable_seat)(struct libseat *, void *);
};
struct libseat *libseat_open_seat(const struct libseat_seat_listener *, void *);
void libseat_close_seat(struct libseat *);
int libseat_get_fd(struct libseat *);
int libseat_dispatch(struct libseat *, int);
int libseat_get_vt(struct libseat *);
int libseat_open_device(struct libseat *, const char *, int *);
int libseat_close_device(struct libseat *, int);
int libseat_disable_seat(struct libseat *);
int libseat_switch_session(struct libseat *, int);
typedef void (*libseat_log_handler)(enum libseat_log_level level, const char *fmt, va_list ap);
void libseat_set_log_handler(libseat_log_handler handler);
void libseat_set_log_level(enum libseat_log_level);
EOF

cat > "$DEPS_DIR/libseat-stub.c" << 'EOF'
#include <stddef.h>
#include "libseat.h"
struct libseat *libseat_open_seat(const struct libseat_seat_listener *l, void *d) { return NULL; }
void libseat_close_seat(struct libseat *s) {}
int libseat_get_fd(struct libseat *s) { return -1; }
int libseat_dispatch(struct libseat *s, int t) { return 0; }
int libseat_get_vt(struct libseat *s) { return -1; }
int libseat_open_device(struct libseat *s, const char *p, int *fd) { return -1; }
int libseat_close_device(struct libseat *s, int id) { return 0; }
int libseat_disable_seat(struct libseat *s) { return 0; }
int libseat_switch_session(struct libseat *s, int n) { return 0; }
void libseat_set_log_handler(libseat_log_handler handler) { (void)handler; }
void libseat_set_log_level(enum libseat_log_level l) {}
EOF
cc -I"$STUB_INCDIR" -dynamiclib -o "$STUB_LIBDIR/libseat.dylib" "$DEPS_DIR/libseat-stub.c" \
    -install_name "$STUB_LIBDIR/libseat.dylib"

echo "Stub libraries built in $STUB_LIBDIR"

# ============================================================
# 3. Create pkg-config files
# ============================================================
echo "=== Creating pkg-config files ==="

cat > "$STUB_PCDIR/libdrm.pc" << EOF
prefix=${SCRIPT_DIR}
exec_prefix=\${prefix}
libdir=${SHIM_BUILD}
includedir=${SCRIPT_DIR}/shims/include
Name: libdrm
Description: DRM via wayland-mac shim
Version: 2.4.108
Libs: -L\${libdir} -lwayland-mac
Cflags: -I\${includedir} -I${STUB_INCDIR}
EOF

cat > "$STUB_PCDIR/gbm.pc" << EOF
prefix=${SCRIPT_DIR}
exec_prefix=\${prefix}
libdir=${SHIM_BUILD}
includedir=${SCRIPT_DIR}/shims/include
Name: gbm
Description: GBM via wayland-mac shim
Version: 22.0.0
Libs: -L\${libdir} -lwayland-mac
Cflags: -I\${includedir}
Requires: libdrm
EOF

for pkg in wayland-server wayland-client wayland-cursor wayland-egl; do
    case "$pkg" in wayland-server) desc="Server" ;; wayland-client) desc="Client" ;; wayland-cursor) desc="Cursor" ;; wayland-egl) desc="EGL" ;; esac
    cat > "$STUB_PCDIR/$pkg.pc" << EOF
prefix=/opt/local
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include
Name: Wayland $desc
Description: Wayland $desc library
Version: 1.22.0
Libs: -L\${libdir} -l$pkg
Cflags: -I\${includedir}
EOF
done

cat > "$STUB_PCDIR/wayland-scanner.pc" << EOF
prefix=/opt/local
exec_prefix=\${prefix}
bindir=\${prefix}/bin
wayland_scanner=\${bindir}/wayland-scanner
Name: Wayland Scanner
Description: Wayland scanner
Version: 1.23.0
Cflags: -I\${prefix}/include
EOF

# liblink maps pkg name to linker -l flag (strip "lib" prefix since -l adds it)
for pkg in libudev:udev libinput:input libevdev:evdev libseat:seat; do
    name="${pkg%%:*}"
    link="${pkg##*:}"
    ver="2.0.0"
    [ "$name" = "libudev" ] && ver="255"
    [ "$name" = "libinput" ] && ver="1.0.0"
    cat > "$STUB_PCDIR/$name.pc" << EOF
prefix=${STUB_LIBDIR}/..
exec_prefix=\${prefix}
libdir=${STUB_LIBDIR}
includedir=${STUB_INCDIR}
Name: $name
Description: $name stub
Version: $ver
Libs: -L\${libdir} -l$link
Cflags: -I\${includedir}
EOF
done

HW_DATADIR="$STUB_PCDIR/../share/hwdata"
mkdir -p "$HW_DATADIR"
touch "$HW_DATADIR/pnp.ids"
cat > "$STUB_PCDIR/hwdata.pc" << EOF
prefix=${STUB_PCDIR}/..
datarootdir=\${prefix}/share
pkgdatadir=${HW_DATADIR}
Name: hwdata
Description: hwdata stub
Version: 0.390
EOF

echo "pkg-config files created in $STUB_PCDIR"

# ============================================================
# 4. Configure and build weston
# ============================================================
echo "=== Configuring weston ==="
export PKG_CONFIG_PATH="$STUB_PCDIR:/opt/local/lib/pkgconfig"
export CFLAGS="-I$STUB_INCDIR -DHAVE_MKOSTEMP=1 -DHAVE_STRCHRNUL=1 -DHAVE_INITGROUPS=1"
export LDFLAGS="-Wl,-undefined,dynamic_lookup"

meson setup "$BUILD_DIR" "$WESTON_DIR" --reconfigure \
    -Dbackend-drm=true \
    -Dbackend-headless=true \
    -Dbackend-wayland=false \
    -Dbackend-x11=false \
    -Dbackend-rdp=false \
    -Dbackend-vnc=false \
    -Dbackend-pipewire=false \
    -Dbackend-default=drm \
    -Drenderer-gl=true \
    -Drenderer-vulkan=false \
    -Dshell-desktop=true \
    -Dshell-ivi=false \
    -Dshell-kiosk=true \
    -Dshell-lua=false \
    -Dxwayland=false \
    -Ddemo-clients=true \
    -Dsimple-clients=shm \
    -Dtests=false \
    -Dcolor-management-lcms=false \
    -Dimage-jpeg=false \
    -Dimage-webp=false \
    -Dresize-pool=false \
    -Dsystemd=false \
    -Db_lundef=false \
    -Dc_args="$CFLAGS -I${SCRIPT_DIR}/shims/gbm/include -I${SCRIPT_DIR}/build/shims/epoll/install-include -Dprogram_invocation_short_name=getprogname() $FORCE_INCLUDE" \
    -Dc_link_args="$LDFLAGS" \
    2>&1 | tee "$BUILD_DIR/meson-setup.log"

echo "=== Building weston ==="
meson compile -C "$BUILD_DIR" 2>&1 | tee "$BUILD_DIR/meson-build.log"

echo ""
echo "=== Done ==="
echo "Binaries in: $BUILD_DIR"
echo "Run: DYLD_INSERT_LIBRARIES=$SHIM_DYLIB $BUILD_DIR/weston"
