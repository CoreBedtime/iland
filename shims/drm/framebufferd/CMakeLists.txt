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
  "-framework Metal"
)

# ── Compile Metal shader into .metallib ──────────────────────────────────
set(METAL_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/src/composite.metal")
set(METAL_AIR    "${CMAKE_BINARY_DIR}/composite.air")
set(METAL_LIB    "${CMAKE_BINARY_DIR}/composite.metallib")

add_custom_command(
  OUTPUT  "${METAL_AIR}"
  COMMAND xcrun metal -c "${METAL_SHADER}" -o "${METAL_AIR}"
  DEPENDS "${METAL_SHADER}"
  COMMENT "[framebufferd] Compiling Metal shader → .air"
)

add_custom_command(
  OUTPUT  "${METAL_LIB}"
  COMMAND xcrun metallib "${METAL_AIR}" -o "${METAL_LIB}"
  DEPENDS "${METAL_AIR}"
  COMMENT "[framebufferd] Linking Metal library → .metallib"
)

add_custom_target(framebufferd_metallib ALL DEPENDS "${METAL_LIB}")
add_dependencies(framebufferd framebufferd_metallib)

