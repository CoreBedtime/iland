add_library(symrez STATIC SymRez.c)

target_include_directories(symrez PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
