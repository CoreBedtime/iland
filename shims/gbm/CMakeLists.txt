add_library(gbm STATIC src/gbm.m)

target_include_directories(gbm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(gbm PUBLIC
  displaysurface
  "-framework CoreFoundation"
  "-framework IOSurface"
)
