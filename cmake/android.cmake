# Android frontend: SDL2 host in runner/android/android_main.c.
# Included from CMakeLists.txt when building with the Android NDK toolchain.
#
# The Gradle project (android/) passes -DDKC1_SDL2_DIR=<SDL2 source tree>.
# SDL2 is built from source as a shared library (libSDL2.so); this file
# builds the game as libmain.so, which SDL's Java glue loads and runs via
# SDL_main().

if(NOT DEFINED DKC1_SDL2_DIR OR NOT EXISTS "${DKC1_SDL2_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Set -DDKC1_SDL2_DIR to an SDL2 source checkout (release-2.30.x).")
endif()

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${DKC1_SDL2_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/sdl2")

# The shared desktop launcher is shell-based and not used on Android (the same
# three files are removed for iOS).
list(REMOVE_ITEM DKC1_ENGINE_SOURCES
    "${SNESRECOMP_ROOT}/runner/src/launcher.c"
    "${SNESRECOMP_ROOT}/runner/src/launcher_cache.c"
    "${SNESRECOMP_ROOT}/runner/src/launcher_picker.c")

# SDL's Java glue loads "libmain.so", so the target must be named "main".
add_library(main SHARED
    ${DKC1_ENGINE_SOURCES}
    ${DKC1_SNESRECOMP_GEN_SOURCES}
    ${DKC1_RUNNER_SOURCES}
    runner/headless_host.c
    runner/dkc1_invariant_monitor.c
    runner/dkc1_video.c
    runner/dkc1_margin_proxy.c
    runner/dkc1_ws_trace.c
    runner/windows_present.c
    runner/win32_host.c
    runner/windows_platform.c
    runner/sdl_host.c
    runner/headless_main.c
    runner/dkc1_hd_scene.c
    runner/dkc1_hd_sprites.c
    runner/dkc1_game.c
    runner/dkc1_framegen.c
    runner/desktop_graphics.c
    runner/desktop_crt.c
    runner/ios/ios_audio_ring.c
    runner/ios/ios_viewport.c
    runner/android/android_main.c)

target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/runner
    ${CMAKE_CURRENT_SOURCE_DIR}/runner/ios
    ${CMAKE_CURRENT_SOURCE_DIR}/recomp
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    ${DKC1_SDL2_DIR}/include)

target_compile_definitions(main PRIVATE
    SNESRECOMP_TRACE=0
    SNESRECOMP_REVERSE_DEBUG=0
    SNESRECOMP_EXTERNAL_RAM_ROUTINE_GUARDS=1
    SYSTEM_VOLUME_MIXER_AVAILABLE=0
    DKC1_BUILD_COMMIT="${DKC1_BUILD_COMMIT}"
    DKC1_BUILD_CONFIG="android")

target_compile_options(main PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wno-unused-function>)

target_link_libraries(main PRIVATE
    SDL2
    ${SNESRECOMP_RUNNER_LIBRARIES}
    log
    android)
