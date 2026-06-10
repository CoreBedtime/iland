cmake_minimum_required(VERSION 3.16)
project(amfree C ASM)

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_VER
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT GIT_VER)
  set(GIT_VER "unknown")
endif()

file(GLOB EXCEPTION_SRCS exception/*.c)

add_executable(amfree
  ${EXCEPTION_SRCS}
  shellcode/hook_entry.S
  shellcode/hook_body.c
)

set_source_files_properties(shellcode/hook_body.c PROPERTIES
  COMPILE_FLAGS "-fno-stack-protector -fno-builtin"
)

target_include_directories(amfree PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/shellcode
)

target_compile_definitions(amfree PRIVATE
  GIT_VERSION="${GIT_VER}"
)

target_compile_options(amfree PRIVATE
  -Wall -Wextra
)

target_link_libraries(amfree PRIVATE
  -lobjc
)

set_target_properties(amfree PROPERTIES
  OUTPUT_NAME amfree
)
