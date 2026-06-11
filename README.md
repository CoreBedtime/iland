# iland

Run Linux Wayland compositors on macOS via `DYLD_INSERT_LIBRARIES`.

Intercepts DRM / GBM / libseat / libudev / libinput / libevdev / EGL APIs, backed by IOSurface + CAWindowServer. Requires **SIP disabled**.

---

## Dependencies

<details open>
<summary><b>MacPorts</b> (required)</summary>

```
sudo port install angle wayland wayland-protocols pixman \
  libxkbcommon pkgconfig meson ninja
```

</details>

## Compositors

<details open>
<summary><b>Weston</b></summary>

| Action | Command |
|---|---|
| **Build from scratch** | `rm -rf weston build build-weston && sh compile.sh && sh build-weston.sh` |
| **Incremental build** | `sh compile.sh && sh build-weston.sh` |
| **Run** | `sh run-weston.sh` |

</details>

---

## Requirements

| Requirement | Why |
|---|---|
| **SIP disabled** | `DYLD_INSERT_LIBRARIES` + Dobby runtime code patching |
| **Root** | `getuid() == 0` checked in dylib constructor |

---

## How it works

```
┌──────────────────────────────────────────────────┐
│               Weston (Wayland compositor)        │
├──────────┬─────────┬──────────┬──────────────────┤
│  DRM     │  GBM    │  libseat │  libudev         │
│ (atomic) │(IOSurf) │  (stub)  │  (stub)           │
├──────────┴─────────┴──────────┴──────────────────┤
│              libwayland-mac.dylib                 │
│     (Dobby hooks, Mach IPC, epoll kqueue-shim)    │
├──────────────────────────────────────────────────┤
│              macOS Kernel / IOSurface             │
│              CAWindowServer / CGDisplay           │
└──────────────────────────────────────────────────┘
```

---

## Architecture

| Component | Role |
|---|---|
| `shims/wayland-mac.c` | Constructor, Dobby hooks, process spawning, binary extraction |
| `shims/drm/` | Atomic modeset, page-flip via pipe event, plist-based resolution |
| `shims/gbm/` | GBM surface backed by IOSurface |
| `shims/egl/` | ANGLE pbuffer + glReadPixels → IOSurface (no R/B swap) |
| `shims/epoll/` | kqueue-backed epoll shim |
| `shims/framebufferd/` | Mach IPC service, CAWindowServer display |
| `shims/amfiexceptiond/` | Patches AMFI at runtime |

---

> macOS + Wayland