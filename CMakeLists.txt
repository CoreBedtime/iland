cmake_minimum_required(VERSION 3.14)
project(myland LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# All dependencies build as static so they can be absorbed into libwayland-mac
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
  dobby
  GIT_REPOSITORY https://github.com/jmpews/Dobby.git
  GIT_TAG master
  GIT_SHALLOW TRUE
)
set(Dobby_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(Dobby_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(dobby)

add_subdirectory(shims/epoll)
add_subdirectory(shims/drm)

# --- Output dylib: absorbs all shims, embeds amfiexceptiond + framebufferd ---
add_library(wayland-mac SHARED shims/wayland-mac.c)

target_link_libraries(wayland-mac PRIVATE
  -Wl,-force_load,$<TARGET_FILE:epoll-shim-interpose>
  -Wl,-force_load,$<TARGET_FILE:dobby>
  -Wl,-force_load,$<TARGET_FILE:drm>
)

# Expose drm's public headers to consumers of wayland-mac
target_link_libraries(wayland-mac PUBLIC drm)

target_link_options(wayland-mac PRIVATE
  -Wl,-sectcreate,__DATA_OBJ,amfiexceptiond,$<TARGET_FILE:amfiexceptiond>
  -Wl,-sectcreate,__DATA_OBJ,framebufferd,$<TARGET_FILE:framebufferd>
)

add_dependencies(wayland-mac
  epoll-shim-interpose
  dobby
  drm
  amfiexceptiond
  framebufferd
)

add_subdirectory(test)
