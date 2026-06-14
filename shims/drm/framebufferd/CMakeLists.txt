add_executable(framebufferd src/main.m src/DisplaySurface.m)

target_include_directories(framebufferd PRIVATE
  ${CMAKE_SOURCE_DIR}/shims/drm/drm/include
)

target_link_libraries(framebufferd PRIVATE
  symrez
  -lobjc
  "-framework Foundation"
  "-framework QuartzCore"
  "-framework IOSurface"
  "-framework IOKit"
  "-framework CoreGraphics"
  "-framework CoreImage"
)

