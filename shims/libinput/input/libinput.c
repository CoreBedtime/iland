#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <libudev.h>
#include <libinput.h>
#include <input_ipc.h>

/* ── Internal struct definitions ────────────────────────────────────── */

struct libinput_event_keyboard {
    uint32_t key;
    int32_t  key_state;
    uint32_t seat_key_count;
    uint64_t time_usec;
};

struct libinput_event_pointer {
    double    dx, dy;
    double    dx_unaccel, dy_unaccel;
    uint32_t  button;
    int32_t   button_state;
    uint32_t  seat_button_count;
    uint64_t  time_usec;
    double    axis_value[2];
    int32_t   axis_source;
};

struct libinput_event_touch {
    int32_t  seat_slot;
    uint64_t time_usec;
    double   x, y;
};

struct libinput_event_tablet_tool {
    uint64_t time;
    double   x, y, pressure, distance, tilt_x, tilt_y;
    int      tip_state, proximity_state;
    uint32_t button, button_state;
    struct libinput_tablet_tool *tool;
};

struct libinput {
    int     dummy;
    void   *user_data;
};

struct libinput_seat {
    char logical_name[64];
};

struct libinput_device {
    int               id;
    int               capabilities;
    char              name[64];
    char              sysname[64];
    unsigned int      vendor, product;
    struct libinput_seat seat;
    void             *user_data;
    int               refcount;
};

struct libinput_event {
    int                     type;
    struct libinput_device *device;
    struct libinput_event  *next;
    union {
        struct libinput_event_keyboard  keyboard;
        struct libinput_event_pointer   pointer;
        struct libinput_event_touch     touch;
        struct libinput_event_tablet_tool tablet;
    };
};

/* ── Internal context ───────────────────────────────────────────────── */

#define MAX_DEVICES 8

static struct {
    struct libinput       ctx;
    struct libinput_device devices[MAX_DEVICES];
    int                   num_devices;
    struct libinput_event *ev_head;
    struct libinput_event *ev_tail;
    pthread_mutex_t       ev_lock;
    int                   pipe_r, pipe_w;
    volatile bool         running;
    bool                  connected;
    int                   pressed_keys;
    int                   pressed_buttons;
    uint32_t              button_state_mask;
    uint8_t               key_state_map[1024 / 8];
} g;

static void queue_event(struct libinput_event *ev);
static struct libinput_event *alloc_event(int type, struct libinput_device *dev);
static struct libinput_device *find_or_create_device(int id, int caps, const char *name);

// ── Helpers for SDL injection ──────────────────────────────────────────

static struct libinput_device *find_or_create_device(int id, int caps, const char *name) {
    for (int i = 0; i < g.num_devices; i++) {
        if (g.devices[i].id == id) return &g.devices[i];
    }
    if (g.num_devices >= MAX_DEVICES) return NULL;
    struct libinput_device *dev = &g.devices[g.num_devices++];
    memset(dev, 0, sizeof(*dev));
    dev->id = id;
    dev->capabilities = caps;
    if (name) strncpy(dev->name, name, sizeof(dev->name)-1);
    else snprintf(dev->name, sizeof(dev->name), "device%d", id);
    snprintf(dev->sysname, sizeof(dev->sysname), "event%d", id);
    snprintf(dev->seat.logical_name, sizeof(dev->seat.logical_name), "seat0");
    return dev;
}

static void ensure_devices(void) {
    if (g.num_devices == 0) {
        struct libinput_device *kb = find_or_create_device(0, INPUT_IPC_CAP_KEYBOARD, "SDL Keyboard");
        struct libinput_device *ptr = find_or_create_device(1, INPUT_IPC_CAP_POINTER, "SDL Pointer");
        (void)kb; (void)ptr;
        // Queue DEVICE_ADDED events
        if (kb) {
            struct libinput_event *ev = alloc_event(LIBINPUT_EVENT_DEVICE_ADDED, kb);
            if (ev) queue_event(ev);
        }
        if (ptr) {
            struct libinput_event *ev = alloc_event(LIBINPUT_EVENT_DEVICE_ADDED, ptr);
            if (ev) queue_event(ev);
        }
    }
}

// SDL backend will call these to inject input
void libinput_sdl_inject_device_added(void) {
    ensure_devices();
}

void libinput_sdl_inject_key(uint32_t scancode, int pressed, uint64_t time_usec) {
    ensure_devices();
    uint16_t key = scancode & 0x3FF;
    int byte = key / 8, bit = key % 8;
    if (byte >= (int)sizeof(g.key_state_map)) return;
    int already_pressed = (g.key_state_map[byte] >> bit) & 1;
    int state_changed = 0;
    if (pressed) {
        if (!already_pressed) {
            g.pressed_keys++;
            g.key_state_map[byte] |= (1 << bit);
            state_changed = 1;
        }
    } else {
        if (already_pressed) {
            g.pressed_keys--;
            g.key_state_map[byte] &= ~(1 << bit);
            state_changed = 1;
        }
    }
    if (!state_changed) return;
    struct libinput_device *dev = find_or_create_device(0, INPUT_IPC_CAP_KEYBOARD, "SDL Keyboard");
    struct libinput_event *ev = alloc_event(LIBINPUT_EVENT_KEYBOARD_KEY, dev);
    if (ev) {
        ev->keyboard.key = scancode;
        ev->keyboard.key_state = pressed ? LIBINPUT_KEY_STATE_PRESSED : LIBINPUT_KEY_STATE_RELEASED;
        ev->keyboard.seat_key_count = g.pressed_keys;
        ev->keyboard.time_usec = time_usec;
        queue_event(ev);
    }
}

void libinput_sdl_inject_motion(double dx, double dy, uint64_t time_usec) {
    ensure_devices();
    struct libinput_device *dev = find_or_create_device(1, INPUT_IPC_CAP_POINTER, "SDL Pointer");
    struct libinput_event *ev = alloc_event(LIBINPUT_EVENT_POINTER_MOTION, dev);
    if (ev) {
        ev->pointer.dx = dx;
        ev->pointer.dy = dy;
        ev->pointer.dx_unaccel = dx;
        ev->pointer.dy_unaccel = dy;
        ev->pointer.time_usec = time_usec;
        queue_event(ev);
    }
}

void libinput_sdl_inject_button(int button, int pressed, uint64_t time_usec) {
    ensure_devices();
    uint32_t btn_bit = 1u << (button & 31);
    int already_pressed = (g.button_state_mask & btn_bit) != 0;
    int state_changed = 0;
    if (pressed) {
        if (!already_pressed) {
            g.pressed_buttons++;
            g.button_state_mask |= btn_bit;
            state_changed = 1;
        }
    } else {
        if (already_pressed) {
            g.pressed_buttons--;
            g.button_state_mask &= ~btn_bit;
            state_changed = 1;
        }
    }
    if (!state_changed) return;
    struct libinput_device *dev = find_or_create_device(1, INPUT_IPC_CAP_POINTER, "SDL Pointer");
    struct libinput_event *ev = alloc_event(LIBINPUT_EVENT_POINTER_BUTTON, dev);
    if (ev) {
        ev->pointer.button = button;
        ev->pointer.button_state = pressed ? LIBINPUT_BUTTON_STATE_PRESSED : LIBINPUT_BUTTON_STATE_RELEASED;
        ev->pointer.seat_button_count = g.pressed_buttons;
        ev->pointer.time_usec = time_usec;
        queue_event(ev);
    }
}

void libinput_sdl_inject_axis(int axis, double value, uint64_t time_usec) {
    ensure_devices();
    struct libinput_device *dev = find_or_create_device(1, INPUT_IPC_CAP_POINTER, "SDL Pointer");
    struct libinput_event *ev = alloc_event(LIBINPUT_EVENT_POINTER_AXIS, dev);
    if (ev) {
        if (axis >=0 && axis <2) ev->pointer.axis_value[axis] = value;
        ev->pointer.axis_source = LIBINPUT_POINTER_AXIS_SOURCE_FINGER;
        ev->pointer.time_usec = time_usec;
        queue_event(ev);
    }
}

/* ── Event queue ────────────────────────────────────────────────────── */

static struct libinput_event *alloc_event(int type, struct libinput_device *dev)
{
    struct libinput_event *ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = type;
    ev->device = dev;
    return ev;
}

static void queue_event(struct libinput_event *ev)
{
    pthread_mutex_lock(&g.ev_lock);
    if (!g.ev_head) {
        g.ev_head = g.ev_tail = ev;
    } else {
        g.ev_tail->next = ev;
        g.ev_tail = ev;
    }
    pthread_mutex_unlock(&g.ev_lock);

    if (g.pipe_w >= 0) {
        char c = 1;
        write(g.pipe_w, &c, 1);
    }
}

/* ── Connect to inputd via Mach IPC ──────────────────────────────────── */

/* ── libinput API implementation ─────────────────────────────────────── */

// Forward declare SDL backend init (optional lazy init)
__attribute__((weak)) void sdl_backend_init(void);

struct libinput *libinput_udev_create_context(
    const struct libinput_interface *iface, void *user_data, struct udev *udev)
{
    (void)iface;
    (void)udev;

    if (g.pipe_r <= 0) {
        if (g.pipe_r == 0) g.pipe_r = -1;
        int p[2];
        if (pipe(p) == 0) {
            g.pipe_r = p[0];
            g.pipe_w = p[1];
            fcntl(g.pipe_r, F_SETFL, fcntl(g.pipe_r, F_GETFL) | O_NONBLOCK);
        }
    }

    if (!g.connected) {
        g.connected = true;
        g.running = true;
        pthread_mutex_init(&g.ev_lock, NULL);
        // Ensure SDL devices exist
        ensure_devices();
        // Lazily init SDL backend so window appears even if weston hasn't done a page flip yet
        if (sdl_backend_init) sdl_backend_init();
    }

    memset(&g.ctx, 0, sizeof(g.ctx));
    g.ctx.user_data = user_data;
    return &g.ctx;
}

struct libinput *libinput_unref(struct libinput *l)
{
    (void)l;
    g.running = false;
    if (g.pipe_r >= 0) { close(g.pipe_r); g.pipe_r = -1; }
    if (g.pipe_w >= 0) { close(g.pipe_w); g.pipe_w = -1; }
    return NULL;
}

int libinput_udev_assign_seat(struct libinput *l, const char *seat)
{
    (void)l;
    (void)seat;
    return 0;
}

int libinput_get_fd(struct libinput *l)
{
    (void)l;
    return g.pipe_r >= 0 ? g.pipe_r : -1;
}

int libinput_dispatch(struct libinput *l)
{
    (void)l;
    if (g.pipe_r >= 0) {
        char buf[64];
        while (read(g.pipe_r, buf, sizeof(buf)) > 0) {}
    }
    return 0;
}

struct libinput_event *libinput_get_event(struct libinput *l)
{
    (void)l;
    pthread_mutex_lock(&g.ev_lock);
    struct libinput_event *ev = g.ev_head;
    if (ev) {
        g.ev_head = ev->next;
        if (!g.ev_head) g.ev_tail = NULL;
        fprintf(stderr, "[libinput] get_event type=%d\n", ev->type);
    }
    pthread_mutex_unlock(&g.ev_lock);
    return ev;
}

void libinput_event_destroy(struct libinput_event *ev)
{
    free(ev);
}

struct libinput *libinput_event_get_context(struct libinput_event *ev)
{
    (void)ev;
    return &g.ctx;
}

enum libinput_event_type libinput_event_get_type(struct libinput_event *ev)
{
    return (enum libinput_event_type)ev->type;
}

struct libinput_device *libinput_event_get_device(struct libinput_event *ev)
{
    return ev->device;
}

int libinput_suspend(struct libinput *l)
{
    (void)l;
    return 0;
}

int libinput_resume(struct libinput *l)
{
    (void)l;
    return 0;
}

/* ── Device functions ──────────────────────────────────────────────────── */

struct libinput_device *libinput_device_ref(struct libinput_device *d)
{
    if (d) d->refcount++;
    return d;
}

struct libinput_device *libinput_device_unref(struct libinput_device *d)
{
    if (d && --d->refcount <= 0) {}
    return NULL;
}

void libinput_device_set_user_data(struct libinput_device *d, void *p)
{
    if (d) d->user_data = p;
}

void *libinput_device_get_user_data(struct libinput_device *d)
{
    return d ? d->user_data : NULL;
}

int libinput_device_has_capability(struct libinput_device *d,
                                   enum libinput_capability c)
{
    if (!d) return 0;
    int mask = 0;
    switch (c) {
    case LIBINPUT_DEVICE_CAP_KEYBOARD: mask = INPUT_IPC_CAP_KEYBOARD; break;
    case LIBINPUT_DEVICE_CAP_POINTER:  mask = INPUT_IPC_CAP_POINTER;  break;
    case LIBINPUT_DEVICE_CAP_TOUCH:    mask = INPUT_IPC_CAP_TOUCH;    break;
    default: return 0;
    }
    return (d->capabilities & mask) != 0;
}

const char *libinput_device_get_name(struct libinput_device *d)
{
    return d ? d->name : "stub";
}

const char *libinput_device_get_sysname(struct libinput_device *d)
{
    return d ? d->sysname : "stub";
}

const char *libinput_device_get_output_name(struct libinput_device *d)
{
    (void)d;
    return NULL;
}

unsigned int libinput_device_get_id_product(struct libinput_device *d)
{
    return d ? d->product : 0;
}

unsigned int libinput_device_get_id_vendor(struct libinput_device *d)
{
    return d ? d->vendor : 0;
}

struct libinput_seat *libinput_device_get_seat(struct libinput_device *d)
{
    return d ? &d->seat : NULL;
}

struct udev_device *libinput_device_get_udev_device(struct libinput_device *d)
{
    (void)d;
    return NULL;
}

/* ── Device config functions (all stubs) ──────────────────────────────── */

int libinput_device_config_tap_set_enabled(struct libinput_device *d, int e)
{ (void)d;(void)e; return 0; }

int libinput_device_config_tap_get_finger_count(struct libinput_device *d)
{ (void)d; return 0; }

int libinput_device_config_tap_set_drag_enabled(struct libinput_device *d, int e)
{ (void)d;(void)e; return 0; }

int libinput_device_config_tap_set_drag_lock_enabled(struct libinput_device *d, int e)
{ (void)d;(void)e; return 0; }

int libinput_device_config_calibration_has_matrix(struct libinput_device *d)
{ (void)d; return 0; }

int libinput_device_config_calibration_set_matrix(struct libinput_device *d,
                                                  const float m[9])
{ (void)d;(void)m; return 0; }

int libinput_device_config_calibration_get_matrix(struct libinput_device *d,
                                                  float m[9])
{ (void)d;(void)m; return 0; }

int libinput_device_config_calibration_get_default_matrix(
    struct libinput_device *d, float m[9])
{ (void)d;(void)m; return 0; }

int libinput_device_config_left_handed_is_available(struct libinput_device *d)
{ (void)d; return 0; }

int libinput_device_config_left_handed_set(struct libinput_device *d, int e)
{ (void)d;(void)e; return 0; }

int libinput_device_config_middle_emulation_is_available(
    struct libinput_device *d)
{ (void)d; return 0; }

int libinput_device_config_middle_emulation_set_enabled(
    struct libinput_device *d, int e)
{ (void)d;(void)e; return 0; }

int libinput_device_config_dwt_is_available(struct libinput_device *d)
{ (void)d; return 0; }

int libinput_device_config_dwt_set_enabled(struct libinput_device *d, int e)
{ (void)d;(void)e; return 0; }

int libinput_device_config_rotation_is_available(struct libinput_device *d)
{ (void)d; return 0; }

int libinput_device_config_rotation_set_angle(struct libinput_device *d,
                                              unsigned int a)
{ (void)d;(void)a; return 0; }

int libinput_device_config_accel_is_available(struct libinput_device *d)
{ (void)d; return 0; }

enum libinput_config_status libinput_device_config_accel_set_speed(
    struct libinput_device *d, double s)
{ (void)d;(void)(int)s; return LIBINPUT_CONFIG_STATUS_UNSUPPORTED; }

enum libinput_config_accel_profile libinput_device_config_accel_get_profiles(
    struct libinput_device *d)
{ (void)d; return LIBINPUT_CONFIG_ACCEL_PROFILE_NONE; }

enum libinput_config_status libinput_device_config_accel_set_profile(
    struct libinput_device *d, enum libinput_config_accel_profile p)
{ (void)d;(void)(int)p; return LIBINPUT_CONFIG_STATUS_UNSUPPORTED; }

enum libinput_config_scroll_method libinput_device_config_scroll_get_methods(
    struct libinput_device *d)
{ (void)d; return LIBINPUT_CONFIG_SCROLL_NO_SCROLL; }

enum libinput_config_status libinput_device_config_scroll_set_method(
    struct libinput_device *d, enum libinput_config_scroll_method m)
{ (void)d;(void)(int)m; return LIBINPUT_CONFIG_STATUS_UNSUPPORTED; }

int libinput_device_config_scroll_set_button(struct libinput_device *d,
                                             uint32_t b)
{ (void)d;(void)b; return 0; }

int libinput_device_config_scroll_has_natural_scroll(
    struct libinput_device *d)
{ (void)d; return 0; }

int libinput_device_config_scroll_set_natural_scroll_enabled(
    struct libinput_device *d, int e)
{ (void)d;(void)e; return 0; }

int libinput_device_led_update(struct libinput_device *d, enum libinput_led l)
{ (void)d;(void)(int)l; return 0; }

/* ── Seat functions ────────────────────────────────────────────────────── */

const char *libinput_seat_get_logical_name(struct libinput_seat *s)
{
    return s ? s->logical_name : "default";
}

/* ── Event accessor functions ─────────────────────────────────────────── */

struct libinput_event_keyboard *libinput_event_get_keyboard_event(
    struct libinput_event *ev)
{
    return &ev->keyboard;
}

uint32_t libinput_event_keyboard_get_key(struct libinput_event_keyboard *e)
{
    return e->key;
}

int libinput_event_keyboard_get_key_state(struct libinput_event_keyboard *e)
{
    return e->key_state;
}

uint32_t libinput_event_keyboard_get_seat_key_count(
    struct libinput_event_keyboard *e)
{
    return e->seat_key_count;
}

uint64_t libinput_event_keyboard_get_time_usec(
    struct libinput_event_keyboard *e)
{
    return e->time_usec;
}

struct libinput_event_pointer *libinput_event_get_pointer_event(
    struct libinput_event *ev)
{
    return &ev->pointer;
}

double libinput_event_pointer_get_dx(struct libinput_event_pointer *e)
{
    return e->dx;
}

double libinput_event_pointer_get_dy(struct libinput_event_pointer *e)
{
    return e->dy;
}

double libinput_event_pointer_get_dx_unaccelerated(
    struct libinput_event_pointer *e)
{
    return e->dx_unaccel;
}

double libinput_event_pointer_get_dy_unaccelerated(
    struct libinput_event_pointer *e)
{
    return e->dy_unaccel;
}

double libinput_event_pointer_get_absolute_x_transformed(
    struct libinput_event_pointer *e, uint32_t w)
{
    (void)w;
    return e->dx;
}

double libinput_event_pointer_get_absolute_y_transformed(
    struct libinput_event_pointer *e, uint32_t h)
{
    (void)h;
    return e->dy;
}

uint32_t libinput_event_pointer_get_button(struct libinput_event_pointer *e)
{
    return e->button;
}

int libinput_event_pointer_get_button_state(struct libinput_event_pointer *e)
{
    return e->button_state;
}

uint32_t libinput_event_pointer_get_seat_button_count(
    struct libinput_event_pointer *e)
{
    return e->seat_button_count;
}

uint64_t libinput_event_pointer_get_time_usec(
    struct libinput_event_pointer *e)
{
    return e->time_usec;
}

int libinput_event_pointer_has_axis(struct libinput_event_pointer *e,
                                    enum libinput_pointer_axis a)
{
    return e->axis_value[a] != 0.0 ? 1 : 0;
}

double libinput_event_pointer_get_axis_value(
    struct libinput_event_pointer *e, enum libinput_pointer_axis a)
{
    return e->axis_value[a];
}

double libinput_event_pointer_get_axis_value_discrete(
    struct libinput_event_pointer *e, enum libinput_pointer_axis a)
{
    return e->axis_value[a];
}

enum libinput_pointer_axis_source libinput_event_pointer_get_axis_source(
    struct libinput_event_pointer *e)
{
    return e->axis_source;
}

struct libinput_event_touch *libinput_event_get_touch_event(
    struct libinput_event *ev)
{
    return &ev->touch;
}

int32_t libinput_event_touch_get_seat_slot(struct libinput_event_touch *e)
{
    return e->seat_slot;
}

uint64_t libinput_event_touch_get_time_usec(struct libinput_event_touch *e)
{
    return e->time_usec;
}

double libinput_event_touch_get_x_transformed(
    struct libinput_event_touch *e, uint32_t w)
{
    (void)w;
    return e->x;
}

double libinput_event_touch_get_y_transformed(
    struct libinput_event_touch *e, uint32_t h)
{
    (void)h;
    return e->y;
}

/* ── Tablet tool functions (all stubs) ─────────────────────────────────── */

struct libinput_event_tablet_tool *libinput_event_get_tablet_tool_event(
    struct libinput_event *ev)
{
    (void)ev;
    return NULL;
}

double libinput_event_tablet_tool_get_x_transformed(
    struct libinput_event_tablet_tool *e, uint32_t w)
{ (void)e;(void)w; return 0; }

double libinput_event_tablet_tool_get_y_transformed(
    struct libinput_event_tablet_tool *e, uint32_t h)
{ (void)e;(void)h; return 0; }

double libinput_event_tablet_tool_get_pressure(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

double libinput_event_tablet_tool_get_distance(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

double libinput_event_tablet_tool_get_tilt_x(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

double libinput_event_tablet_tool_get_tilt_y(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

int libinput_event_tablet_tool_get_tip_state(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

int libinput_event_tablet_tool_get_proximity_state(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

uint32_t libinput_event_tablet_tool_get_button(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

uint32_t libinput_event_tablet_tool_get_button_state(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

uint64_t libinput_event_tablet_tool_get_time(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

int libinput_event_tablet_tool_x_has_changed(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

int libinput_event_tablet_tool_y_has_changed(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

int libinput_event_tablet_tool_pressure_has_changed(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

int libinput_event_tablet_tool_distance_has_changed(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

int libinput_event_tablet_tool_tilt_x_has_changed(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

int libinput_event_tablet_tool_tilt_y_has_changed(
    struct libinput_event_tablet_tool *e)
{ (void)e; return 0; }

struct libinput_tablet_tool *libinput_event_tablet_tool_get_tool(
    struct libinput_event_tablet_tool *e)
{ (void)e; return NULL; }

enum libinput_tablet_tool_type libinput_tablet_tool_get_type(
    struct libinput_tablet_tool *t)
{ (void)t; return 0; }

uint64_t libinput_tablet_tool_get_tool_id(struct libinput_tablet_tool *t)
{ (void)t; return 0; }

uint64_t libinput_tablet_tool_get_serial(struct libinput_tablet_tool *t)
{ (void)t; return 0; }

int libinput_tablet_tool_is_unique(struct libinput_tablet_tool *t)
{ (void)t; return 0; }

int libinput_tablet_tool_has_pressure(struct libinput_tablet_tool *t)
{ (void)t; return 0; }

int libinput_tablet_tool_has_distance(struct libinput_tablet_tool *t)
{ (void)t; return 0; }

int libinput_tablet_tool_has_tilt(struct libinput_tablet_tool *t)
{ (void)t; return 0; }

void libinput_tablet_tool_set_user_data(struct libinput_tablet_tool *t,
                                        void *d)
{ (void)t;(void)d; }

void *libinput_tablet_tool_get_user_data(struct libinput_tablet_tool *t)
{ (void)t; return NULL; }

/* ── Misc functions ────────────────────────────────────────────────────── */

void *libinput_get_user_data(struct libinput *l)
{
    return l ? l->user_data : NULL;
}

void libinput_log_set_handler(struct libinput *l, libinput_log_handler h)
{
    (void)l;(void)(void*)h;
}

void libinput_log_set_priority(struct libinput *l, enum libinput_log_priority p)
{
    (void)l;(void)(int)p;
}
