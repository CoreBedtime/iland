add_library(sdl-backend STATIC src/sdl_backend.m)

target_include_directories(sdl-backend PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/shims/libinput/include
)

target_link_libraries(sdl-backend PUBLIC
  "-framework Foundation"
  "-framework AppKit"
  "-framework IOSurface"
  "-framework CoreGraphics"
  "-F/System/Library/PrivateFrameworks"
  "-framework SkyLight"
)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL3 REQUIRED sdl3)
  # Use static libs to get framework dependencies on macOS
  target_include_directories(sdl-backend PUBLIC ${SDL3_INCLUDE_DIRS})
  target_compile_options(sdl-backend PUBLIC ${SDL3_CFLAGS_OTHER})
  target_link_directories(sdl-backend PUBLIC ${SDL3_LIBRARY_DIRS})
  # Link SDL3 plus its macOS framework dependencies (from pkg-config static)
  if(SDL3_STATIC_LIBRARIES)
    target_link_libraries(sdl-backend PUBLIC ${SDL3_STATIC_LIBRARIES})
  else()
    target_link_libraries(sdl-backend PUBLIC ${SDL3_LIBRARIES})
    # Explicitly add common macOS frameworks required by SDL3
    target_link_libraries(sdl-backend PUBLIC
      "-framework Cocoa" "-framework IOKit" "-framework CoreVideo"
      "-framework CoreAudio" "-framework AudioToolbox" "-framework AVFoundation"
      "-framework Foundation" "-framework GameController" "-framework Metal"
      "-framework QuartzCore" "-framework Carbon" "-framework ForceFeedback"
      "-framework CoreMedia"
    )
  endif()
  if(SDL3_STATIC_LDFLAGS)
    target_link_options(sdl-backend PUBLIC ${SDL3_STATIC_LDFLAGS})
  elseif(SDL3_LDFLAGS)
    target_link_options(sdl-backend PUBLIC ${SDL3_LDFLAGS})
  endif()
else()
  target_include_directories(sdl-backend PUBLIC /opt/local/include)
  target_link_directories(sdl-backend PUBLIC /opt/local/lib)
  target_link_libraries(sdl-backend PUBLIC SDL3
    "-framework Cocoa" "-framework IOKit" "-framework CoreVideo"
    "-framework CoreAudio" "-framework AudioToolbox" "-framework AVFoundation"
    "-framework GameController" "-framework Metal" "-framework QuartzCore"
  )
endif()
