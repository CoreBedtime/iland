add_library(libinput STATIC
    libinput.c
)
target_include_directories(libinput PUBLIC
    ${CMAKE_SOURCE_DIR}/shims/libinput/include
    ${CMAKE_SOURCE_DIR}/shims/udev/include
    ${CMAKE_SOURCE_DIR}/shims/include
)
