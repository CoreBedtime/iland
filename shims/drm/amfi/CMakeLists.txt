add_executable(amfiexceptiond src/exception.m)

target_compile_options(amfiexceptiond PRIVATE
  -Wall -Wextra
  -Wno-deprecated-declarations
)

target_link_libraries(amfiexceptiond PRIVATE
  -lobjc
  "-framework Foundation"
)

add_custom_command(TARGET amfiexceptiond POST_BUILD
  COMMAND codesign -s - $<TARGET_FILE:amfiexceptiond>
  COMMENT "Adhoc codesigning amfiexceptiond"
)
