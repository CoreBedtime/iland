add_library(drm STATIC src/drm.c)

target_include_directories(drm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
