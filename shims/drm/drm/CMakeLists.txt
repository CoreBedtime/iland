add_library(drm STATIC src/drm.c src/drm_linux.c src/drm_ioctl.c)

target_include_directories(drm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/shims/include
)

target_link_libraries(drm PUBLIC
  displaysurface
  "-framework CoreFoundation"
  "-framework IOSurface"
  "-framework CoreGraphics"
)
