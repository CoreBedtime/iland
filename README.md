# myland

Shim layer for running Wayland compositors and clients on macOS.

Each shim in `shims/` hooks macOS system calls via [Dobby](https://github.com/jmpews/Dobby) to provide Linux APIs needed by Wayland.

## Usage

```
export DYLD_INSERT_LIBRARIES=/path/to/libepoll-shim-interpose.dylib
```

**System Integrity Protection (SIP) must be disabled** for `DYLD_INSERT_LIBRARIES` and runtime code patching (used by Dobby) to function. Without disabling SIP, the shim will not work.

On Apple Silicon, you will also need to disable AMFI or use a debugged build.

**DRM access is restricted to root only.** IOMFB acces requires root privileges and AppleMobileFileIntegrity (AMFI) exceptions to access. To put it simpy, the
drm shim will not function without running as root. Whatever this consumes this library, must run as root.

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Shims

| Directory | Description |
|-----------|-------------|
| shims/epoll | epoll, eventfd, timerfd, signalfd via kqueue |
| shims/drm   | DRM/KMS ioctl forwarding to macOS IOKit |
