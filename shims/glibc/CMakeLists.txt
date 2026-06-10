add_library(glibc-compat STATIC
    src/glibc-compat.c
)

target_include_directories(glibc-compat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# funopen is a BSD extension; suppress -Wdeprecated-declarations on macOS
target_compile_options(glibc-compat PRIVATE
    -Wno-deprecated-declarations
)
