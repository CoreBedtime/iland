#include <dobby.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include "epoll_shim_ctx.h"

typeof(read) *wrap_real_read;
typeof(write) *wrap_real_write;
typeof(close) *wrap_real_close;
typeof(poll) *wrap_real_poll;
#if !defined(__APPLE__)
typeof(ppoll) *wrap_real_ppoll;
#endif
typeof(fcntl) *wrap_real_fcntl;

static ssize_t
hooked_read(int fd, void *buf, size_t nbytes)
{
  return epoll_shim_read(fd, buf, nbytes);
}

static ssize_t
hooked_write(int fd, void const *buf, size_t nbytes)
{
  return epoll_shim_write(fd, buf, nbytes);
}

static int
hooked_close(int fd)
{
  return epoll_shim_close(fd);
}

static int
hooked_poll(struct pollfd fds[], nfds_t nfds, int timeout)
{
  return epoll_shim_poll(fds, nfds, timeout);
}

#if !defined(__APPLE__)
static int
hooked_ppoll(struct pollfd fds[], nfds_t nfds,
    struct timespec const *restrict timeout,
    sigset_t const *restrict newsigmask)
{
  return epoll_shim_ppoll(fds, nfds, timeout, newsigmask);
}
#endif

static int
hooked_fcntl(int fd, int cmd, ...)
{
  va_list ap;
  va_start(ap, cmd);
  void *arg = va_arg(ap, void *);
  int rv = epoll_shim_fcntl(fd, cmd, arg);
  va_end(ap);
  return rv;
}

static void
hook_init_impl(void)
{
#define HOOK(fun)                                                         \
  do {                                                                    \
    int ret = DobbyHook((void *)fun, (void *)hooked_##fun,                \
                        (void **)&wrap_real_##fun);                       \
    if (ret != 0) {                                                       \
      fprintf(stderr,                                                     \
          "epoll-shim: error hooking \"" #fun "\" with DobbyHook!\n");    \
      abort();                                                            \
    }                                                                     \
  } while (0)

  HOOK(read);
  HOOK(write);
  HOOK(close);
  HOOK(poll);
#if !defined(__APPLE__)
  HOOK(ppoll);
#endif
  HOOK(fcntl);

#undef HOOK
}

static __attribute__((constructor)) void
hook_init(void)
{
  hook_init_impl();
}
