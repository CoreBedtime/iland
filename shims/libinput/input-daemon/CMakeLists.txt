add_executable(inputd inputd.c)
target_include_directories(inputd PRIVATE
    ${CMAKE_SOURCE_DIR}/shims/libinput/include
)

add_custom_command(TARGET inputd POST_BUILD
  COMMAND codesign -s - --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/entitlements.plist $<TARGET_FILE:inputd>
  COMMENT "Adhoc codesigning inputd with entitlements"
)