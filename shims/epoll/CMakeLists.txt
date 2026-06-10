cmake_minimum_required(VERSION 3.14)
project(epoll-shim LANGUAGES C)

option(BUILD_SHARED_LIBS "build shim as shared lib" ON)
option(ENABLE_COMPILER_WARNINGS "enable compiler warnings" OFF)

if(ENABLE_COMPILER_WARNINGS)
  add_compile_options(
    "-Wall"
    "-Wextra"
    "-Wconversion"
    "-Wsign-conversion"
    "-Wmissing-prototypes"
    "-Werror=implicit-function-declaration"
    "-Werror=return-type"
    "-Werror=incompatible-pointer-types")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

add_subdirectory(external)

include(CheckTypeSize)
list(APPEND CMAKE_EXTRA_INCLUDE_FILES "errno.h" "stdlib.h")
check_type_size(errno_t ERRNO_T BUILTIN_TYPES_ONLY LANGUAGE C)
if(NOT HAVE_ERRNO_T)
  add_definitions(-Derrno_t=int)
endif()

add_subdirectory(src)
