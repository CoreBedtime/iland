add_library(egl-shim STATIC src/egl.c)

target_include_directories(egl-shim PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  /opt/local/include
)

target_link_libraries(egl-shim PUBLIC
  gbm
  "-framework IOSurface"
  "-framework CoreFoundation"
  "-framework Accelerate"
)
