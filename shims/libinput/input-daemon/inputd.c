#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <mach/mach.h>
#include <bootstrap.h>
#include <mach/mach_time.h>
#include <input_ipc.h>

#include <hidapi/hidapi.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>

#define LIBINPUT_KEY_STATE_RELEASED 0
#define LIBINPUT_KEY_STATE_PRESSED 1
#define LIBINPUT_BUTTON_STATE_RELEASED 0
#define LIBINPUT_BUTTON_STATE_PRESSED 1

#define MAX_CLIENTS 4
#define MAX_HID_USAGE 0x100

static volatile bool g_running = true;
static mach_port_t   g_server_port = MACH_PORT_NULL;
static mach_port_t   g_clients[MAX_CLIENTS];
static int           g_num_clients;
static pthread_mutex_t g_client_lock = PTHREAD_MUTEX_INITIALIZER;

static void handle_signal(int sig)
{
    (void)sig;
    g_running = false;
}

static IOHIDManagerRef g_hid_manager;

static pthread_t g_mach_thread;
static pthread_t g_hid_thread;

enum {
    KEY_RESERVED   = 0,   KEY_ESC       = 1,   KEY_1         = 2,
    KEY_2         = 3,   KEY_3         = 4,   KEY_4         = 5,
    KEY_5         = 6,   KEY_6         = 7,   KEY_7         = 8,
    KEY_8         = 9,   KEY_9         = 10,  KEY_0         = 11,
    KEY_MINUS     = 12,  KEY_EQUAL     = 13,  KEY_BACKSPACE = 14,
    KEY_TAB       = 15,  KEY_Q         = 16,  KEY_W         = 17,
    KEY_E         = 18,  KEY_R         = 19,  KEY_T         = 20,
    KEY_Y         = 21,  KEY_U         = 22,  KEY_I         = 23,
    KEY_O         = 24,  KEY_P         = 25,  KEY_LEFTBRACE = 26,
    KEY_RIGHTBRACE= 27,  KEY_ENTER     = 28,  KEY_LEFTCTRL  = 29,
    KEY_A         = 30,  KEY_S         = 31,  KEY_D         = 32,
    KEY_F         = 33,  KEY_G         = 34,  KEY_H         = 35,
    KEY_J         = 36,  KEY_K         = 37,  KEY_L         = 38,
    KEY_SEMICOLON = 39,  KEY_APOSTROPHE= 40,  KEY_GRAVE     = 41,
    KEY_LEFTSHIFT = 42,  KEY_BACKSLASH = 43,  KEY_Z         = 44,
    KEY_X         = 45,  KEY_C         = 46,  KEY_V         = 47,
    KEY_B         = 48,  KEY_N         = 49,  KEY_M         = 50,
    KEY_COMMA     = 51,  KEY_DOT       = 52,  KEY_SLASH     = 53,
    KEY_RIGHTSHIFT= 54,  KEY_KPASTERISK= 55,  KEY_LEFTALT   = 56,
    KEY_SPACE     = 57,  KEY_CAPSLOCK  = 58,
    KEY_F1        = 59,  KEY_F2        = 60,  KEY_F3        = 61,
    KEY_F4        = 62,  KEY_F5        = 63,  KEY_F6        = 64,
    KEY_F7        = 65,  KEY_F8        = 66,  KEY_F9        = 67,
    KEY_F10       = 68,  KEY_NUMLOCK   = 69,  KEY_SCROLLLOCK= 70,
    KEY_KP7       = 71,  KEY_KP8       = 72,  KEY_KP9       = 73,
    KEY_KPMINUS   = 74,  KEY_KP4       = 75,  KEY_KP5       = 76,
    KEY_KP6       = 77,  KEY_KPPLUS    = 78,  KEY_KP1       = 79,
    KEY_KP2       = 80,  KEY_KP3       = 81,  KEY_KP0       = 82,
    KEY_KPDOT     = 83,  KEY_102ND     = 86,
    KEY_F11       = 87,  KEY_F12       = 88,
    KEY_KPENTER   = 96,  KEY_RIGHTCTRL = 97,  KEY_KPSLASH   = 98,
    KEY_SYSRQ     = 99,  KEY_RIGHTALT  = 100, KEY_LINEFEED  = 101,
    KEY_HOME      = 102, KEY_UP        = 103, KEY_PAGEUP    = 104,
    KEY_LEFT      = 105, KEY_RIGHT     = 106, KEY_END       = 107,
    KEY_DOWN      = 108, KEY_PAGEDOWN  = 109, KEY_INSERT    = 110,
    KEY_DELETE    = 111, KEY_MUTE      = 113,
    KEY_VOLUMEUP  = 115, KEY_VOLUMEDOWN= 114,
    KEY_POWER     = 116, KEY_KPEQUAL   = 117,
    KEY_PAUSE     = 119, KEY_KPCOMMA   = 121,
    KEY_LEFTMETA  = 125, KEY_RIGHTMETA = 126, KEY_COMPOSE   = 127,
    KEY_STOP      = 128, KEY_AGAIN     = 129, KEY_PROPS     = 130,
    KEY_UNDO      = 131, KEY_FRONT     = 132, KEY_COPY      = 133,
    KEY_OPEN      = 134, KEY_PASTE     = 135, KEY_FIND      = 136,
    KEY_CUT       = 137, KEY_HELP      = 138, KEY_MENU      = 139,
    KEY_CALC      = 140, KEY_SELECT    = 141, KEY_SLEEP     = 142,
    KEY_BOOKMARKS = 156, KEY_BACK      = 158, KEY_FORWARD   = 159,
    KEY_REFRESH   = 173,
    KEY_SEARCH    = 217,
    KEY_BRIGHTNESSDOWN = 224, KEY_BRIGHTNESSUP = 225,
    KEY_F13       = 183, KEY_F14       = 184, KEY_F15       = 185,
    KEY_F16       = 186, KEY_F17       = 187, KEY_F18       = 188,
    KEY_F19       = 189, KEY_F20       = 190, KEY_F21       = 191,
    KEY_F22       = 192, KEY_F23       = 193, KEY_F24       = 194,
    BTN_MISC      = 0x100, BTN_0      = 0x100,
    BTN_1         = 0x101, BTN_2      = 0x102,
    BTN_LEFT      = 0x110, BTN_RIGHT  = 0x111,
    BTN_MIDDLE    = 0x112, BTN_SIDE   = 0x113,
    BTN_EXTRA     = 0x114, BTN_FORWARD= 0x115,
    BTN_BACK      = 0x116, BTN_TASK   = 0x117,
    BTN_TOUCH     = 0x14a,
};

static const uint16_t hid_to_evdev[MAX_HID_USAGE] = {
    [0x00] = KEY_RESERVED,    [0x01] = KEY_RESERVED,
    [0x02] = KEY_RESERVED,    [0x03] = KEY_RESERVED,
    [0x04] = KEY_A,           [0x05] = KEY_B,
    [0x06] = KEY_C,           [0x07] = KEY_D,
    [0x08] = KEY_E,           [0x09] = KEY_F,
    [0x0A] = KEY_G,           [0x0B] = KEY_H,
    [0x0C] = KEY_I,           [0x0D] = KEY_J,
    [0x0E] = KEY_K,           [0x0F] = KEY_L,
    [0x10] = KEY_M,           [0x11] = KEY_N,
    [0x12] = KEY_O,           [0x13] = KEY_P,
    [0x14] = KEY_Q,           [0x15] = KEY_R,
    [0x16] = KEY_S,           [0x17] = KEY_T,
    [0x18] = KEY_U,           [0x19] = KEY_V,
    [0x1A] = KEY_W,           [0x1B] = KEY_X,
    [0x1C] = KEY_Y,           [0x1D] = KEY_Z,
    [0x1E] = KEY_1,           [0x1F] = KEY_2,
    [0x20] = KEY_3,           [0x21] = KEY_4,
    [0x22] = KEY_5,           [0x23] = KEY_6,
    [0x24] = KEY_7,           [0x25] = KEY_8,
    [0x26] = KEY_9,           [0x27] = KEY_0,
    [0x28] = KEY_ENTER,       [0x29] = KEY_ESC,
    [0x2A] = KEY_BACKSPACE,   [0x2B] = KEY_TAB,
    [0x2C] = KEY_SPACE,       [0x2D] = KEY_MINUS,
    [0x2E] = KEY_EQUAL,       [0x2F] = KEY_LEFTBRACE,
    [0x30] = KEY_RIGHTBRACE,  [0x31] = KEY_BACKSLASH,
    [0x32] = KEY_BACKSLASH,   [0x33] = KEY_SEMICOLON,
    [0x34] = KEY_APOSTROPHE,  [0x35] = KEY_GRAVE,
    [0x36] = KEY_COMMA,       [0x37] = KEY_DOT,
    [0x38] = KEY_SLASH,       [0x39] = KEY_CAPSLOCK,
    [0x3A] = KEY_F1,          [0x3B] = KEY_F2,
    [0x3C] = KEY_F3,          [0x3D] = KEY_F4,
    [0x3E] = KEY_F5,          [0x3F] = KEY_F6,
    [0x40] = KEY_F7,          [0x41] = KEY_F8,
    [0x42] = KEY_F9,          [0x43] = KEY_F10,
    [0x44] = KEY_F11,         [0x45] = KEY_F12,
    [0x46] = KEY_SYSRQ,       [0x47] = KEY_SCROLLLOCK,
    [0x48] = KEY_PAUSE,       [0x49] = KEY_INSERT,
    [0x4A] = KEY_HOME,        [0x4B] = KEY_PAGEUP,
    [0x4C] = KEY_DELETE,      [0x4D] = KEY_END,
    [0x4E] = KEY_PAGEDOWN,    [0x4F] = KEY_RIGHT,
    [0x50] = KEY_LEFT,        [0x51] = KEY_DOWN,
    [0x52] = KEY_UP,          [0x53] = KEY_NUMLOCK,
    [0x54] = KEY_KPSLASH,     [0x55] = KEY_KPASTERISK,
    [0x56] = KEY_KPMINUS,     [0x57] = KEY_KPPLUS,
    [0x58] = KEY_KPENTER,     [0x59] = KEY_KP1,
    [0x5A] = KEY_KP2,         [0x5B] = KEY_KP3,
    [0x5C] = KEY_KP4,         [0x5D] = KEY_KP5,
    [0x5E] = KEY_KP6,         [0x5F] = KEY_KP7,
    [0x60] = KEY_KP8,         [0x61] = KEY_KP9,
    [0x62] = KEY_KP0,         [0x63] = KEY_KPDOT,
    [0x64] = KEY_102ND,       [0x65] = KEY_COMPOSE,
    [0x66] = KEY_POWER,       [0x67] = KEY_KPEQUAL,
    [0x68] = KEY_F13,         [0x69] = KEY_F14,
    [0x6A] = KEY_F15,         [0x6B] = KEY_F16,
    [0x6C] = KEY_F17,         [0x6D] = KEY_F18,
    [0x6E] = KEY_F19,         [0x6F] = KEY_F20,
    [0x70] = KEY_F21,         [0x71] = KEY_F22,
    [0x72] = KEY_F23,         [0x73] = KEY_F24,
    [0x74] = KEY_OPEN,        [0x75] = KEY_HELP,
    [0x76] = KEY_MENU,        [0x77] = KEY_SELECT,
    [0xE0] = KEY_LEFTCTRL,    [0xE1] = KEY_LEFTSHIFT,
    [0xE2] = KEY_LEFTALT,     [0xE3] = KEY_LEFTMETA,
    [0xE4] = KEY_RIGHTCTRL,   [0xE5] = KEY_RIGHTSHIFT,
    [0xE6] = KEY_RIGHTALT,    [0xE7] = KEY_RIGHTMETA,
};

static int hid_usage_to_evdev(uint32_t usage_page, uint32_t usage)
{
    if (usage_page == 0x07 && usage < MAX_HID_USAGE)
        return hid_to_evdev[usage];
    if (usage_page == 0x09 && usage >= 1 && usage <= 32)
        return BTN_LEFT + (int)(usage - 1);
    return 0;
}

static uint64_t now_usec(void)
{
    static mach_timebase_info_data_t tb = {0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t now = mach_absolute_time();
    return (now * tb.numer) / (tb.denom * 1000);
}

static int send_event(input_ipc_event_t *msg)
{
    pthread_mutex_lock(&g_client_lock);
    int count = g_num_clients;
    for (int i = 0; i < count; i++) {
        msg->header.msgh_remote_port = g_clients[i];
        kern_return_t kr = mach_msg(&msg->header, MACH_SEND_MSG,
                                     sizeof(*msg), 0, MACH_PORT_NULL,
                                     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "[inputd] send to client %d: %s\n", i,
                    mach_error_string(kr));
        }
    }
    pthread_mutex_unlock(&g_client_lock);
    return 0;
}

static void send_device_added(int id, int caps, const char *name)
{
    input_ipc_event_t msg = {0};
    msg.header.msgh_bits      = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_id         = INPUT_IPC_EVENT_ID;
    msg.header.msgh_size       = sizeof(msg);
    msg.body.msgh_descriptor_count = 0;
    msg.event_type   = INPUT_IPC_EVENT_DEVICE_ADDED;
    msg.device_id    = id;
    msg.device_caps  = caps;
    msg.time_usec    = now_usec();
    strncpy(msg.device_name, name, sizeof(msg.device_name) - 1);
    send_event(&msg);
}

static void send_device_removed(int id)
{
    input_ipc_event_t msg = {0};
    msg.header.msgh_bits      = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_id         = INPUT_IPC_EVENT_ID;
    msg.header.msgh_size       = sizeof(msg);
    msg.body.msgh_descriptor_count = 0;
    msg.event_type   = INPUT_IPC_EVENT_DEVICE_REMOVED;
    msg.device_id    = id;
    send_event(&msg);
}

static void send_key_event(uint64_t time, int key, int pressed)
{
    input_ipc_event_t msg = {0};
    msg.header.msgh_bits      = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_id         = INPUT_IPC_EVENT_ID;
    msg.header.msgh_size       = sizeof(msg);
    msg.body.msgh_descriptor_count = 0;
    msg.event_type   = INPUT_IPC_EVENT_KEYBOARD_KEY;
    msg.device_id    = 0;
    msg.time_usec    = time;
    msg.key          = key;
    msg.key_state    = pressed ? LIBINPUT_KEY_STATE_PRESSED
                               : LIBINPUT_KEY_STATE_RELEASED;
    send_event(&msg);
}

static void send_motion_event(uint64_t time, double dx, double dy)
{
    input_ipc_event_t msg = {0};
    msg.header.msgh_bits      = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_id         = INPUT_IPC_EVENT_ID;
    msg.header.msgh_size       = sizeof(msg);
    msg.body.msgh_descriptor_count = 0;
    msg.event_type   = INPUT_IPC_EVENT_POINTER_MOTION;
    msg.device_id    = 1;
    msg.time_usec    = time;
    msg.pointer_dx   = dx;
    msg.pointer_dy   = dy;
    send_event(&msg);
}

static void send_button_event(uint64_t time, int btn, int pressed)
{
    input_ipc_event_t msg = {0};
    msg.header.msgh_bits      = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_id         = INPUT_IPC_EVENT_ID;
    msg.header.msgh_size       = sizeof(msg);
    msg.body.msgh_descriptor_count = 0;
    msg.event_type   = INPUT_IPC_EVENT_POINTER_BUTTON;
    msg.device_id    = 1;
    msg.time_usec    = time;
    msg.pointer_button = btn;
    msg.pointer_button_state = pressed ? LIBINPUT_BUTTON_STATE_PRESSED
                                        : LIBINPUT_BUTTON_STATE_RELEASED;
    send_event(&msg);
}

static void send_scroll_event(uint64_t time, int axis, double value)
{
    input_ipc_event_t msg = {0};
    msg.header.msgh_bits      = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.header.msgh_local_port = MACH_PORT_NULL;
    msg.header.msgh_id         = INPUT_IPC_EVENT_ID;
    msg.header.msgh_size       = sizeof(msg);
    msg.body.msgh_descriptor_count = 0;
    msg.event_type   = INPUT_IPC_EVENT_POINTER_AXIS;
    msg.device_id    = 1;
    msg.time_usec    = time;
    msg.pointer_axis = axis;
    msg.pointer_axis_value = value;
    send_event(&msg);
}

static pthread_mutex_t g_accel_lock = PTHREAD_MUTEX_INITIALIZER;
static double g_accel_dx, g_accel_dy;

static void flush_motion(void)
{
    pthread_mutex_lock(&g_accel_lock);
    double dx = g_accel_dx;
    double dy = g_accel_dy;
    g_accel_dx = g_accel_dy = 0.0;
    pthread_mutex_unlock(&g_accel_lock);

    if (dx != 0.0 || dy != 0.0)
        send_motion_event(now_usec(), dx, dy);
}

static void accum_motion(double dx, double dy)
{
    pthread_mutex_lock(&g_accel_lock);
    g_accel_dx += dx;
    g_accel_dy += dy;
    pthread_mutex_unlock(&g_accel_lock);
}

static void hid_value_cb(void *context, IOReturn result, void *sender,
                          IOHIDValueRef value)
{
    (void)context;(void)result;(void)sender;

    IOHIDElementRef elem = IOHIDValueGetElement(value);
    if (!elem) return;

    uint32_t page  = IOHIDElementGetUsagePage(elem);
    uint32_t usage = IOHIDElementGetUsage(elem);
    CFIndex  val   = IOHIDValueGetIntegerValue(value);
    uint64_t ts    = now_usec();

    if (page == kHIDPage_KeyboardOrKeypad) {
        int evdev = hid_usage_to_evdev(page, (uint32_t)usage);
        if (evdev > 0)
            send_key_event(ts, evdev, (int)val);
    } else if (page == kHIDPage_Button) {
        int btn = BTN_LEFT + (int)(usage - 1);
        send_button_event(ts, btn, (int)val);
    } else if (page == kHIDPage_GenericDesktop) {
        switch (usage) {
        case kHIDUsage_GD_X:
            accum_motion((double)val, 0.0);
            break;
        case kHIDUsage_GD_Y:
            accum_motion(0.0, (double)val);
            break;
        case kHIDUsage_GD_Wheel:
            send_scroll_event(ts, 0, (double)val);
            break;
        }
    } else if (page == kHIDPage_Consumer) {
        int evdev = 0;
        switch (usage) {
        case 0x006F: evdev = KEY_BRIGHTNESSUP;   break;
        case 0x0070: evdev = KEY_BRIGHTNESSDOWN; break;
        case 0x00E2: evdev = KEY_MUTE;           break;
        case 0x00E9: evdev = KEY_VOLUMEUP;       break;
        case 0x00EA: evdev = KEY_VOLUMEDOWN;     break;
        case 0x00B5: evdev = KEY_CALC;           break;
        case 0x0183: evdev = KEY_SLEEP;          break;
        case 0x0192: evdev = KEY_MENU;           break;
        case 0x0221: evdev = KEY_SEARCH;         break;
        case 0x0223: evdev = KEY_HOME;           break;
        case 0x0224: evdev = KEY_BACK;           break;
        case 0x0225: evdev = KEY_FORWARD;        break;
        case 0x0226: evdev = KEY_STOP;           break;
        case 0x0227: evdev = KEY_REFRESH;        break;
        case 0x022A: evdev = KEY_BOOKMARKS;      break;
        }
        if (evdev > 0)
            send_key_event(ts, evdev, (int)val);
    }
}

static void hid_device_matched_cb(void *context, IOReturn result,
                                   void *sender, IOHIDDeviceRef device)
{
    (void)context;(void)result;(void)sender;
    CFStringRef name = IOHIDDeviceGetProperty(device,
                                                CFSTR(kIOHIDProductKey));
    char buf[128] = "Unknown";
    if (name && CFStringGetCString(name, buf, sizeof(buf),
                                    kCFStringEncodingUTF8))
        fprintf(stderr, "[inputd] matched HID device: %s\n", buf);
}

static void hid_device_removed_cb(void *context, IOReturn result,
                                   void *sender, IOHIDDeviceRef device)
{
    (void)context;(void)result;(void)sender;(void)device;
}

static void motion_timer_cb(CFRunLoopTimerRef timer, void *info)
{
    (void)timer;(void)info;
    if (!g_running) {
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    flush_motion();
}

static void *hid_thread(void *arg)
{
    (void)arg;

    g_hid_manager = IOHIDManagerCreate(kCFAllocatorDefault,
                                        kIOHIDManagerOptionNone);
    if (!g_hid_manager) {
        fprintf(stderr, "[inputd] IOHIDManagerCreate failed\n");
        return NULL;
    }

    CFNumberRef page_num;
    CFStringRef page_key = CFSTR(kIOHIDDeviceUsagePageKey);
    void *keys[1] = { (void *)page_key };
    void *vals[1];

    page_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                               &(int32_t){ kHIDPage_KeyboardOrKeypad });
    vals[0] = (void *)page_num;
    CFDictionaryRef keyboard_match = CFDictionaryCreate(
        kCFAllocatorDefault, (const void **)keys, (const void **)vals,
        1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(page_num);

    page_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                               &(int32_t){ kHIDPage_GenericDesktop });
    vals[0] = (void *)page_num;
    CFDictionaryRef pointer_match = CFDictionaryCreate(
        kCFAllocatorDefault, (const void **)keys, (const void **)vals,
        1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(page_num);

    CFTypeRef match_arr[2] = { keyboard_match, pointer_match };
    CFArrayRef matches = CFArrayCreate(
        kCFAllocatorDefault, (const void **)match_arr, 2,
        &kCFTypeArrayCallBacks);
    CFRelease(keyboard_match);
    CFRelease(pointer_match);

    IOHIDManagerSetDeviceMatchingMultiple(g_hid_manager, matches);

    IOHIDManagerRegisterDeviceMatchingCallback(g_hid_manager,
                                                hid_device_matched_cb, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_hid_manager,
                                               hid_device_removed_cb, NULL);
    IOHIDManagerRegisterInputValueCallback(g_hid_manager, hid_value_cb,
                                            NULL);

    IOHIDManagerScheduleWithRunLoop(g_hid_manager, CFRunLoopGetCurrent(),
                                     kCFRunLoopDefaultMode);

    IOReturn ret = IOHIDManagerOpen(g_hid_manager, kIOHIDManagerOptionNone);
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "[inputd] IOHIDManagerOpen: 0x%x\n", ret);
        CFRelease(g_hid_manager);
        return NULL;
    }

    fprintf(stderr, "[inputd] HID capture started\n");

    CFRunLoopTimerRef mtimer = CFRunLoopTimerCreate(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(),
        1.0 / 250.0, 0, 0, motion_timer_cb, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), mtimer, kCFRunLoopCommonModes);

    CFRunLoopRun();

    CFRelease(mtimer);
    IOHIDManagerClose(g_hid_manager, kIOHIDManagerOptionNone);
    CFRelease(g_hid_manager);
    return NULL;
}

static void log_hid_devices(void)
{
    struct hid_device_info *devs = hid_enumerate(0, 0);
    if (!devs) return;

    for (struct hid_device_info *cur = devs; cur; cur = cur->next) {
        char name[128] = "?";
        if (cur->product_string)
            wcstombs(name, cur->product_string, sizeof(name));
        fprintf(stderr, "[inputd]  HID: %s (vid=%04x pid=%04x page=0x%02x "
                        "usage=0x%02x)\n",
                name, cur->vendor_id, cur->product_id,
                cur->usage_page, cur->usage);
    }

    hid_free_enumeration(devs);
}

static void mach_server_thread(void)
{
    while (g_running) {
        union {
            input_ipc_subscribe_t subscribe;
            uint8_t padding[sizeof(input_ipc_subscribe_t) + 64];
        } buf = {0};
        input_ipc_subscribe_t *msg = &buf.subscribe;
        msg->header.msgh_size       = sizeof(buf);
        msg->header.msgh_local_port = g_server_port;

        kern_return_t kr = mach_msg(&msg->header,
                                     MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                     0, sizeof(buf),
                                     g_server_port, 500,
                                     MACH_PORT_NULL);
        if (kr == MACH_RCV_TIMED_OUT) {
            continue;
        }
        if (kr != KERN_SUCCESS) {
            if (g_running)
                fprintf(stderr, "[inputd] mach_msg recv: %s\n",
                        mach_error_string(kr));
            continue;
        }

        if (msg->header.msgh_id != INPUT_IPC_SUBSCRIBE_ID)
            continue;

        if (msg->body.msgh_descriptor_count >= 1 &&
            msg->client_port.type == MACH_MSG_PORT_DESCRIPTOR &&
            msg->client_port.name != MACH_PORT_NULL)
        {
            mach_port_t client_port = msg->client_port.name;

            fprintf(stderr, "[inputd] client subscribed, port=%d\n",
                    client_port);

            pthread_mutex_lock(&g_client_lock);
            if (g_num_clients < MAX_CLIENTS)
                g_clients[g_num_clients++] = client_port;
            pthread_mutex_unlock(&g_client_lock);

            send_device_added(0, INPUT_IPC_CAP_KEYBOARD, "macOS Keyboard");
            send_device_added(1, INPUT_IPC_CAP_POINTER,  "macOS Pointer");

            {
                input_ipc_event_t m = {0};
                m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
                m.header.msgh_remote_port = client_port;
                m.header.msgh_id          = INPUT_IPC_EVENT_ID;
                m.header.msgh_size        = sizeof(m);
                m.event_type              = INPUT_IPC_EVENT_POINTER_MOTION;
                m.device_id               = 1;
                m.pointer_dx              = 0.0;
                m.pointer_dy              = 0.0;
                m.time_usec               = now_usec();
                kern_return_t kr = mach_msg(&m.header, MACH_SEND_MSG,
                                             sizeof(m), 0, MACH_PORT_NULL,
                                             MACH_MSG_TIMEOUT_NONE,
                                             MACH_PORT_NULL);
                if (kr != KERN_SUCCESS)
                    fprintf(stderr, "[inputd] send motion: %s\n",
                            mach_error_string(kr));
            }
        } else {
            fprintf(stderr, "[inputd] subscribe missing port descriptor\n");
        }
    }
}

int main(void)
{
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    if (hid_init() != 0) {
        fprintf(stderr, "[inputd] hid_init failed\n");
        return 1;
    }

    log_hid_devices();

    kern_return_t kr = mach_port_allocate(mach_task_self(),
                                           MACH_PORT_RIGHT_RECEIVE,
                                           &g_server_port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[inputd] mach_port_allocate: %s\n",
                mach_error_string(kr));
        return 1;
    }

    kr = mach_port_insert_right(mach_task_self(), g_server_port,
                                 g_server_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[inputd] mach_port_insert_right: %s\n",
                mach_error_string(kr));
        return 1;
    }

    kr = bootstrap_register(bootstrap_port, INPUT_IPC_SERVICE_NAME,
                             g_server_port);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[inputd] bootstrap_register %s: %s\n",
                INPUT_IPC_SERVICE_NAME, mach_error_string(kr));
        return 1;
    }

    fprintf(stderr, "[inputd] listening on %s\n", INPUT_IPC_SERVICE_NAME);

    pthread_create(&g_hid_thread, NULL, hid_thread, NULL);
    pthread_detach(g_hid_thread);

    mach_server_thread();

    send_device_removed(0);
    send_device_removed(1);

    hid_exit();

    fprintf(stderr, "[inputd] shutting down\n");
    return 0;
}
