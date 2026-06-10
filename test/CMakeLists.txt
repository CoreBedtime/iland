add_executable(test_wayland_mac test_wayland_mac.c)
target_link_libraries(test_wayland_mac PRIVATE wayland-mac)

add_executable(test_drm test_drm.c)
target_link_libraries(test_drm PRIVATE wayland-mac)

add_executable(test_epoll test_epoll.c)
target_link_libraries(test_epoll PRIVATE wayland-mac)
