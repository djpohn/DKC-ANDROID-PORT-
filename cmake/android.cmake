set(DKC1_SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android/SDL" CACHE PATH "SDL2 source dir" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${DKC1_SDL2_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/sdl2" EXCLUDE_FROM_ALL)

file(GLOB DKC1_EXTRA_RUNNER
    "${CMAKE_CURRENT_SOURCE_DIR}/runner/dkc1_*.c"
)

add_library(main SHARED
    ${DKC1_ENGINE_SOURCES}
    ${DKC1_SNESRECOMP_GEN_SOURCES}
    ${DKC1_RUNNER_SOURCES}
    ${DKC1_EXTRA_RUNNER}
    runner/headless_host.c
    runner/desktop_graphics.c
    runner/desktop_crt.c
    runner/desktop_sram.c
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

target_link_libraries(main PRIVATE
    SDL2
    GLESv2
    EGL
    android
    log
    m)
