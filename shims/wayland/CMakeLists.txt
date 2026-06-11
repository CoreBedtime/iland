add_subdirectory(amfi)

add_library(wayland STATIC
    src/wayland-server.c
)

target_include_directories(wayland PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
