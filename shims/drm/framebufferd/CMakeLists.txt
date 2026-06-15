add_executable(framebufferd src/main.m)

target_include_directories(framebufferd PRIVATE
  ${CMAKE_SOURCE_DIR}/shims/drm/drm/include
)

target_link_libraries(framebufferd PRIVATE
  displaysurface
  symrez
  -lobjc
  "-framework Foundation"
  "-framework QuartzCore"
  "-framework IOSurface"
  "-framework IOKit"
  "-framework CoreGraphics"
)

