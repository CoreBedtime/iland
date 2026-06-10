add_library(udev STATIC src/udev.c)

target_include_directories(udev PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
