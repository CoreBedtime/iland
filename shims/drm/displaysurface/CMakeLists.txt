add_library(displaysurface STATIC src/DisplaySurface.m)

target_include_directories(displaysurface PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(displaysurface PUBLIC
  "-framework Foundation"
  "-framework IOSurface"
)
