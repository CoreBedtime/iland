#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <libudev.h>
#include <libinput.h>

static char dummy_libinput_ctx;
static int dummy_libinput_pipe[2] = {-1, -1};

struct libinput *libinput_udev_create_context(const struct libinput_interface *i, void *u, struct udev *ud) {
    (void)i;(void)u;(void)ud;
    if (dummy_libinput_pipe[0] < 0) pipe(dummy_libinput_pipe);
    return (struct libinput *)&dummy_libinput_ctx;
}
struct libinput *libinput_unref(struct libinput *l) { (void)l; return NULL; }
int libinput_udev_assign_seat(struct libinput *l, const char *s) { (void)l;(void)s; return 0; }
int libinput_get_fd(struct libinput *l) { (void)l; return dummy_libinput_pipe[0]; }
int libinput_dispatch(struct libinput *l) { (void)l; return 0; }
struct libinput_event *libinput_get_event(struct libinput *l) { (void)l; return NULL; }
void libinput_event_destroy(struct libinput_event *e) { (void)e; }
struct libinput *libinput_event_get_context(struct libinput_event *e) { (void)e; return NULL; }
enum libinput_event_type libinput_event_get_type(struct libinput_event *e) { (void)e; return 0; }
struct libinput_device *libinput_event_get_device(struct libinput_event *e) { (void)e; return NULL; }
int libinput_suspend(struct libinput *l) { (void)l; return 0; }
int libinput_resume(struct libinput *l) { (void)l; return 0; }
struct libinput_device *libinput_device_ref(struct libinput_device *d) { (void)d; return NULL; }
struct libinput_device *libinput_device_unref(struct libinput_device *d) { (void)d; return NULL; }
void libinput_device_set_user_data(struct libinput_device *d, void *p) { (void)d;(void)p; }
void *libinput_device_get_user_data(struct libinput_device *d) { (void)d; return NULL; }
int libinput_device_has_capability(struct libinput_device *d, enum libinput_capability c) { (void)d;(void)(int)c; return 0; }
const char *libinput_device_get_name(struct libinput_device *d) { (void)d; return "stub"; }
const char *libinput_device_get_sysname(struct libinput_device *d) { (void)d; return "stub"; }
const char *libinput_device_get_output_name(struct libinput_device *d) { (void)d; return NULL; }
unsigned int libinput_device_get_id_product(struct libinput_device *d) { (void)d; return 0; }
unsigned int libinput_device_get_id_vendor(struct libinput_device *d) { (void)d; return 0; }
struct libinput_seat *libinput_device_get_seat(struct libinput_device *d) { (void)d; return NULL; }
struct udev_device *libinput_device_get_udev_device(struct libinput_device *d) { (void)d; return NULL; }
int libinput_device_config_tap_set_enabled(struct libinput_device *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_tap_get_finger_count(struct libinput_device *d) { (void)d; return 0; }
int libinput_device_config_tap_set_drag_enabled(struct libinput_device *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_tap_set_drag_lock_enabled(struct libinput_device *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_calibration_has_matrix(struct libinput_device *d) { (void)d; return 0; }
int libinput_device_config_calibration_set_matrix(struct libinput_device *d, const float m[9]) { (void)d;(void)m; return 0; }
int libinput_device_config_calibration_get_matrix(struct libinput_device *d, float m[9]) { (void)d;(void)m; return 0; }
int libinput_device_config_calibration_get_default_matrix(struct libinput_device *d, float m[9]) { (void)d;(void)m; return 0; }
int libinput_device_config_left_handed_is_available(struct libinput_device *d) { (void)d; return 0; }
int libinput_device_config_left_handed_set(struct libinput_device *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_middle_emulation_is_available(struct libinput_device *d) { (void)d; return 0; }
int libinput_device_config_middle_emulation_set_enabled(struct libinput_device *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_dwt_is_available(struct libinput_device *d) { (void)d; return 0; }
int libinput_device_config_dwt_set_enabled(struct libinput_device *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_config_rotation_is_available(struct libinput_device *d) { (void)d; return 0; }
int libinput_device_config_rotation_set_angle(struct libinput_device *d, unsigned int a) { (void)d;(void)a; return 0; }
int libinput_device_config_accel_is_available(struct libinput_device *d) { (void)d; return 0; }
enum libinput_config_status libinput_device_config_accel_set_speed(struct libinput_device *d, double s) { (void)d;(void)(int)s; return 1; }
enum libinput_config_accel_profile libinput_device_config_accel_get_profiles(struct libinput_device *d) { (void)d; return 0; }
enum libinput_config_status libinput_device_config_accel_set_profile(struct libinput_device *d, enum libinput_config_accel_profile p) { (void)d;(void)(int)p; return 1; }
enum libinput_config_scroll_method libinput_device_config_scroll_get_methods(struct libinput_device *d) { (void)d; return 0; }
enum libinput_config_status libinput_device_config_scroll_set_method(struct libinput_device *d, enum libinput_config_scroll_method m) { (void)d;(void)(int)m; return 1; }
int libinput_device_config_scroll_set_button(struct libinput_device *d, uint32_t b) { (void)d;(void)b; return 0; }
int libinput_device_config_scroll_has_natural_scroll(struct libinput_device *d) { (void)d; return 0; }
int libinput_device_config_scroll_set_natural_scroll_enabled(struct libinput_device *d, int e) { (void)d;(void)e; return 0; }
int libinput_device_led_update(struct libinput_device *d, enum libinput_led l) { (void)d;(void)(int)l; return 0; }
const char *libinput_seat_get_logical_name(struct libinput_seat *s) { (void)s; return "default"; }
struct libinput_event_keyboard *libinput_event_get_keyboard_event(struct libinput_event *e) { (void)e; return NULL; }
uint32_t libinput_event_keyboard_get_key(struct libinput_event_keyboard *e) { (void)e; return 0; }
int libinput_event_keyboard_get_key_state(struct libinput_event_keyboard *e) { (void)e; return 0; }
uint32_t libinput_event_keyboard_get_seat_key_count(struct libinput_event_keyboard *e) { (void)e; return 0; }
uint64_t libinput_event_keyboard_get_time_usec(struct libinput_event_keyboard *e) { (void)e; return 0; }
struct libinput_event_pointer *libinput_event_get_pointer_event(struct libinput_event *e) { (void)e; return NULL; }
double libinput_event_pointer_get_dx(struct libinput_event_pointer *e) { (void)e; return 0; }
double libinput_event_pointer_get_dy(struct libinput_event_pointer *e) { (void)e; return 0; }
double libinput_event_pointer_get_dx_unaccelerated(struct libinput_event_pointer *e) { (void)e; return 0; }
double libinput_event_pointer_get_dy_unaccelerated(struct libinput_event_pointer *e) { (void)e; return 0; }
double libinput_event_pointer_get_absolute_x_transformed(struct libinput_event_pointer *e, uint32_t w) { (void)e;(void)w; return 0; }
double libinput_event_pointer_get_absolute_y_transformed(struct libinput_event_pointer *e, uint32_t h) { (void)e;(void)h; return 0; }
uint32_t libinput_event_pointer_get_button(struct libinput_event_pointer *e) { (void)e; return 0; }
int libinput_event_pointer_get_button_state(struct libinput_event_pointer *e) { (void)e; return 0; }
uint32_t libinput_event_pointer_get_seat_button_count(struct libinput_event_pointer *e) { (void)e; return 0; }
uint64_t libinput_event_pointer_get_time_usec(struct libinput_event_pointer *e) { (void)e; return 0; }
int libinput_event_pointer_has_axis(struct libinput_event_pointer *e, enum libinput_pointer_axis a) { (void)e;(void)(int)a; return 0; }
double libinput_event_pointer_get_axis_value(struct libinput_event_pointer *e, enum libinput_pointer_axis a) { (void)e;(void)(int)a; return 0; }
double libinput_event_pointer_get_axis_value_discrete(struct libinput_event_pointer *e, enum libinput_pointer_axis a) { (void)e;(void)(int)a; return 0; }
enum libinput_pointer_axis_source libinput_event_pointer_get_axis_source(struct libinput_event_pointer *e) { (void)e; return 0; }
struct libinput_event_touch *libinput_event_get_touch_event(struct libinput_event *e) { (void)e; return NULL; }
int32_t libinput_event_touch_get_seat_slot(struct libinput_event_touch *e) { (void)e; return 0; }
uint64_t libinput_event_touch_get_time_usec(struct libinput_event_touch *e) { (void)e; return 0; }
double libinput_event_touch_get_x_transformed(struct libinput_event_touch *e, uint32_t w) { (void)e;(void)w; return 0; }
double libinput_event_touch_get_y_transformed(struct libinput_event_touch *e, uint32_t h) { (void)e;(void)h; return 0; }
struct libinput_event_tablet_tool *libinput_event_get_tablet_tool_event(struct libinput_event *e) { (void)e; return NULL; }
double libinput_event_tablet_tool_get_x_transformed(struct libinput_event_tablet_tool *e, uint32_t w) { (void)e;(void)w; return 0; }
double libinput_event_tablet_tool_get_y_transformed(struct libinput_event_tablet_tool *e, uint32_t h) { (void)e;(void)h; return 0; }
double libinput_event_tablet_tool_get_pressure(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
double libinput_event_tablet_tool_get_distance(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
double libinput_event_tablet_tool_get_tilt_x(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
double libinput_event_tablet_tool_get_tilt_y(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
int libinput_event_tablet_tool_get_tip_state(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
int libinput_event_tablet_tool_get_proximity_state(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
uint32_t libinput_event_tablet_tool_get_button(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
uint32_t libinput_event_tablet_tool_get_button_state(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
uint64_t libinput_event_tablet_tool_get_time(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
int libinput_event_tablet_tool_x_has_changed(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
int libinput_event_tablet_tool_y_has_changed(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
int libinput_event_tablet_tool_pressure_has_changed(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
int libinput_event_tablet_tool_distance_has_changed(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
int libinput_event_tablet_tool_tilt_x_has_changed(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
int libinput_event_tablet_tool_tilt_y_has_changed(struct libinput_event_tablet_tool *e) { (void)e; return 0; }
struct libinput_tablet_tool *libinput_event_tablet_tool_get_tool(struct libinput_event_tablet_tool *e) { (void)e; return NULL; }
enum libinput_tablet_tool_type libinput_tablet_tool_get_type(struct libinput_tablet_tool *t) { (void)t; return 0; }
uint64_t libinput_tablet_tool_get_tool_id(struct libinput_tablet_tool *t) { (void)t; return 0; }
uint64_t libinput_tablet_tool_get_serial(struct libinput_tablet_tool *t) { (void)t; return 0; }
int libinput_tablet_tool_is_unique(struct libinput_tablet_tool *t) { (void)t; return 0; }
int libinput_tablet_tool_has_pressure(struct libinput_tablet_tool *t) { (void)t; return 0; }
int libinput_tablet_tool_has_distance(struct libinput_tablet_tool *t) { (void)t; return 0; }
int libinput_tablet_tool_has_tilt(struct libinput_tablet_tool *t) { (void)t; return 0; }
void libinput_tablet_tool_set_user_data(struct libinput_tablet_tool *t, void *d) { (void)t;(void)d; }
void *libinput_tablet_tool_get_user_data(struct libinput_tablet_tool *t) { (void)t; return NULL; }
void *libinput_get_user_data(struct libinput *l) { (void)l; return NULL; }
void libinput_log_set_handler(struct libinput *l, libinput_log_handler h) { (void)l;(void)(void*)h; }
void libinput_log_set_priority(struct libinput *l, enum libinput_log_priority p) { (void)l;(void)(int)p; }