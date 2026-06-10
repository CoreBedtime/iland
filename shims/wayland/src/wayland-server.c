#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/event.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>

struct wl_event_loop;
struct wl_event_source;

typedef int (*wl_event_loop_signal_func_t)(int signal_number, void *data);
typedef int (*wl_event_loop_timer_func_t)(void *data);
typedef int (*wl_event_loop_idle_func_t)(void *data);

typedef struct wl_event_source *(*wl_event_loop_add_fd_t)(
    struct wl_event_loop *loop, int fd, uint32_t mask,
    int (*callback)(int fd, uint32_t mask, void *data), void *data);
typedef int (*wl_event_source_remove_t)(struct wl_event_source *source);
typedef int (*wl_event_source_fd_update_t)(struct wl_event_source *source,
                                            uint32_t mask);

#define WL_EVENT_READABLE 1
#define WL_EVENT_WRITABLE 2
#define WL_EVENT_ERROR    4
#define WL_EVENT_HANGUP   8

/* ------------------------------------------------------------------ */
/* Signal handling — one shared kqueue, static handler table           */
/* ------------------------------------------------------------------ */
static int signal_kq = -1;
static struct wl_event_source *signal_fd_source;
static pthread_once_t signal_kq_once = PTHREAD_ONCE_INIT;

#define MAX_SIGNALS 8
static struct {
    int sig;
    wl_event_loop_signal_func_t func;
    void *data;
} signal_handlers[MAX_SIGNALS];
static int num_signal_handlers;

static int
signal_dispatch(int fd, uint32_t mask, void *data) {
    (void)mask;(void)data;
    struct kevent ev;
    struct timespec zero = {0, 0};
    while (kevent(fd, NULL, 0, &ev, 1, &zero) > 0) {
        if (ev.filter == EVFILT_SIGNAL) {
            int sig = (int)ev.ident;
            for (int i = 0; i < num_signal_handlers; i++)
                if (signal_handlers[i].sig == sig)
                    signal_handlers[i].func(sig, signal_handlers[i].data);
        }
    }
    return 0;
}

static void
init_signal_kq(void) {
    signal_kq = kqueue();
}

struct wl_event_source *
wl_event_loop_add_signal(struct wl_event_loop *loop,
                         int signal_number,
                         wl_event_loop_signal_func_t func,
                         void *data) {
    pthread_once(&signal_kq_once, init_signal_kq);
    if (signal_kq < 0) return NULL;
    if (num_signal_handlers >= MAX_SIGNALS) return NULL;

    if (!signal_fd_source) {
        wl_event_loop_add_fd_t add_fd =
            (wl_event_loop_add_fd_t)dlsym(RTLD_DEFAULT,
                                            "wl_event_loop_add_fd");
        if (!add_fd) return NULL;
        signal_fd_source = add_fd(loop, signal_kq, WL_EVENT_READABLE,
                                   signal_dispatch, NULL);
        if (!signal_fd_source) return NULL;
    }

    struct kevent kev;
    EV_SET(&kev, signal_number, EVFILT_SIGNAL, EV_ADD | EV_CLEAR,
           0, 0, 0);
    if (kevent(signal_kq, &kev, 1, NULL, 0, NULL) < 0)
        return NULL;

    signal_handlers[num_signal_handlers].sig = signal_number;
    signal_handlers[num_signal_handlers].func = func;
    signal_handlers[num_signal_handlers].data = data;
    num_signal_handlers++;

    return signal_fd_source;
}

/* ------------------------------------------------------------------ */
/* Timer handling — self-contained (doesn't reuse signal kq)           */
/* Each timer gets its own kqueue for independent EVFILT_TIMER control */
/* ------------------------------------------------------------------ */
struct timer_source {
    int kq;
    struct wl_event_source *fd_source;
    wl_event_loop_timer_func_t func;
    void *data;
};

#define MAX_TIMERS 32
static struct timer_source *timers[MAX_TIMERS];
static int num_timers;

static void
untrack_timer(struct timer_source *ts) {
    for (int i = 0; i < num_timers; i++) {
        if (timers[i] == ts) {
            timers[i] = timers[--num_timers];
            return;
        }
    }
}

static struct timer_source *
find_timer_by_source(struct wl_event_source *s) {
    for (int i = 0; i < num_timers; i++)
        if (timers[i]->fd_source == s)
            return timers[i];
    return NULL;
}

static int
timer_dispatch(int fd, uint32_t mask, void *data) {
    (void)mask;
    struct timer_source *ts = (struct timer_source *)data;
    struct kevent ev;
    struct timespec zero = {0, 0};
    while (kevent(fd, NULL, 0, &ev, 1, &zero) > 0) {
        if (ev.filter == EVFILT_TIMER)
            ts->func(ts->data);
    }
    return 0;
}

struct wl_event_source *
wl_event_loop_add_timer(struct wl_event_loop *loop,
                        wl_event_loop_timer_func_t func,
                        void *data) {
    if (num_timers >= MAX_TIMERS) return NULL;

    struct timer_source *ts = calloc(1, sizeof(*ts));
    if (!ts) return NULL;

    ts->kq = kqueue();
    if (ts->kq < 0) { free(ts); return NULL; }

    wl_event_loop_add_fd_t add_fd =
        (wl_event_loop_add_fd_t)dlsym(RTLD_DEFAULT,
                                        "wl_event_loop_add_fd");
    if (!add_fd) { close(ts->kq); free(ts); return NULL; }

    ts->fd_source = add_fd(loop, ts->kq, WL_EVENT_READABLE,
                            timer_dispatch, ts);
    if (!ts->fd_source) { close(ts->kq); free(ts); return NULL; }

    ts->func = func;
    ts->data = data;

    timers[num_timers++] = ts;
    return ts->fd_source;
}

int
wl_event_source_timer_update(struct wl_event_source *source, int ms_delay) {
    struct timer_source *ts = find_timer_by_source(source);
    if (!ts) return -1;

    /* Remove existing timer */
    struct kevent kev;
    EV_SET(&kev, 0, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
    kevent(ts->kq, &kev, 1, NULL, 0, NULL);

    if (ms_delay >= 0) {
        EV_SET(&kev, 0, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
               0, ms_delay, 0);
        if (kevent(ts->kq, &kev, 1, NULL, 0, NULL) < 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* wl_event_source_remove — intercept to clean up timer/signal state   */
/* ------------------------------------------------------------------ */
static int real_remove_inited;
static wl_event_source_remove_t real_remove;

int
wl_event_source_remove(struct wl_event_source *source) {
    if (!real_remove_inited) {
        real_remove = (wl_event_source_remove_t)
            dlsym(RTLD_NEXT, "wl_event_source_remove");
        real_remove_inited = 1;
    }

    struct timer_source *ts = find_timer_by_source(source);
    if (ts) {
        untrack_timer(ts);
        free(ts);
    }

    if (source == signal_fd_source)
        signal_fd_source = NULL;

    if (real_remove)
        return real_remove(source);
    return 0;
}
