cmake_minimum_required(VERSION 3.14)
project(myland LANGUAGES C OBJC)

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
# Override the epoll-shim's -fvisibility=hidden so its symbols are
# exported from the wayland-mac dylib.
set_target_properties(epoll-shim-interpose PROPERTIES
  C_VISIBILITY_PRESET "default"
)
add_subdirectory(shims/drm)
add_subdirectory(shims/gbm)
add_subdirectory(shims/egl)
add_subdirectory(shims/glibc)
add_subdirectory(shims/wayland)
add_subdirectory(shims/libinput)
add_subdirectory(shims/udev)

# --- Output dylib: absorbs all shims, embeds amfiexceptiond + framebufferd + inputd ---
add_library(wayland-mac SHARED shims/wayland-mac.c)

target_link_libraries(wayland-mac PRIVATE
  -Wl,-force_load,$<TARGET_FILE:dobby>
  -Wl,-force_load,$<TARGET_FILE:epoll-shim-interpose>
  -Wl,-force_load,$<TARGET_FILE:glibc-compat>
  -Wl,-force_load,$<TARGET_FILE:wayland>
  -Wl,-force_load,$<TARGET_FILE:drm>
  -Wl,-force_load,$<TARGET_FILE:gbm>
  -Wl,-force_load,$<TARGET_FILE:egl-shim>
  -Wl,-force_load,$<TARGET_FILE:udev>
  -Wl,-force_load,$<TARGET_FILE:libinput>
)

# Expose all shim APIs via the single umbrella header <wayland-mac.h>.
# Shim-specific headers (epoll, drm) are propagated transitively through
# each library target's PUBLIC include directories.
target_link_libraries(wayland-mac PUBLIC drm gbm egl-shim)
target_link_libraries(wayland-mac PRIVATE "-framework Foundation" "-lobjc")
target_include_directories(wayland-mac PRIVATE
  ${CMAKE_SOURCE_DIR}/shims/epoll/src
)
target_include_directories(wayland-mac PUBLIC
  ${CMAKE_SOURCE_DIR}/shims
  ${CMAKE_SOURCE_DIR}/shims/include
  ${CMAKE_SOURCE_DIR}/shims/gbm/include
  ${CMAKE_SOURCE_DIR}/shims/glibc/include
  $<TARGET_PROPERTY:epoll-shim-interpose,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_options(wayland-mac PRIVATE
  -Wl,-rpath,${CMAKE_BINARY_DIR}
  -Wl,-sectcreate,__DATA_OBJ,amfiexceptiond,$<TARGET_FILE:amfiexceptiond>
  -Wl,-sectcreate,__DATA_OBJ,framebufferd,$<TARGET_FILE:framebufferd>
  -Wl,-sectcreate,__DATA_OBJ,inputd,$<TARGET_FILE:inputd>
)

add_dependencies(wayland-mac
  epoll-shim-interpose
  dobby
  drm
  gbm
  egl-shim
  glibc-compat
  wayland
  udev
  amfiexceptiond
  framebufferd
  libinput
  inputd
)

add_subdirectory(test)
