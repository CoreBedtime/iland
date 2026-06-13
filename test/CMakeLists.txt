add_executable(test_wayland_mac test_wayland_mac.c)
target_link_libraries(test_wayland_mac PRIVATE wayland-mac)

add_executable(test_drm test_drm.c)
target_link_libraries(test_drm PRIVATE wayland-mac)

add_executable(test_epoll test_epoll.c)
target_link_libraries(test_epoll PRIVATE wayland-mac)

add_executable(test_drm_system_ties test_drm_system_ties.c)
target_link_libraries(test_drm_system_ties PRIVATE wayland-mac)

add_executable(test_drm_fullscreen test_drm_fullscreen.c)
target_link_libraries(test_drm_fullscreen PRIVATE wayland-mac)

add_executable(test_libinput test_libinput.c)
target_link_libraries(test_libinput PRIVATE wayland-mac)
target_include_directories(test_libinput PRIVATE
    ${CMAKE_SOURCE_DIR}/shims/libinput/include
    ${CMAKE_SOURCE_DIR}/shims/udev/include
)

add_executable(kmscube kmscube.c esUtil.c)
target_include_directories(kmscube PRIVATE
  ${CMAKE_SOURCE_DIR}/test
  /opt/local/include
)
target_compile_options(kmscube PRIVATE
  "-include${CMAKE_SOURCE_DIR}/test/kmscube_compat.h"
)
target_link_libraries(kmscube PRIVATE
  wayland-mac
  "/opt/local/lib/libGLESv2.dylib"
)
