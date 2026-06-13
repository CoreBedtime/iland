add_executable(inputd inputd.c)
target_include_directories(inputd PRIVATE
    ${CMAKE_SOURCE_DIR}/shims/libinput/include
    /opt/local/include
)
target_link_libraries(inputd PRIVATE
    /opt/local/lib/libhidapi.dylib
    "-framework CoreFoundation"
    "-framework IOKit"
)

add_custom_command(TARGET inputd POST_BUILD
  COMMAND codesign -s - --entitlements ${CMAKE_CURRENT_SOURCE_DIR}/entitlements.plist $<TARGET_FILE:inputd>
  COMMENT "Adhoc codesigning inputd with entitlements"
)