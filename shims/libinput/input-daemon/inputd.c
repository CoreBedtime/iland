#include <stdio.h>
#include <unistd.h>
#include <signal.h>

static volatile int g_running = 1;

static void
handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

int
main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    fprintf(stderr, "[inputd] macOS input daemon starting\n");

    while (g_running) {
        pause();
    }

    fprintf(stderr, "[inputd] shutting down\n");
    return 0;
}