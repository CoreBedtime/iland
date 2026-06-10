add_executable(framebufferd src/main.c)
target_link_libraries(framebufferd PRIVATE drm)

add_custom_command(TARGET framebufferd POST_BUILD
  COMMAND codesign -s - --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/entitlements.plist -f $<TARGET_FILE:framebufferd>
  COMMENT "Signing framebufferd with entitlements"
)
