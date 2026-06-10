# myland

Compat layer for running Wayland compositors and clients directly on macOS.

Produces `libwayland-mac.dylib`.

To build, you need these:
```
sudo port install wayland                # XQuartz fork — builds on darwin
sudo port install wayland-protocols
sudo port install pixman
sudo port install libxkbcommon
sudo port install pkgconfig meson ninja
sudo port install angle                  # OpenGL ES implementaition```

**System Integrity Protection (SIP) must be disabled** for `DYLD_INSERT_LIBRARIES` and runtime code patching (used by Dobby) to function. Without disabling SIP, the shim will not work.
