#include <stdio.h>

extern void wayland_mac_init(void);

int main(void) {
    printf("[test] wayland-mac loaded\n");
    wayland_mac_init();
    printf("[test] wayland_mac_init returned\n");
    return 0;
}
