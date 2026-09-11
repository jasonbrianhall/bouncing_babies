cmake_minimum_required(VERSION 3.15)
project(BouncingBabies CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)

# Regenerate generated/music_tracks.h (+ per-track headers, sfx_levelup_mid.h)
# from midi/music/*.mid and midi/sfx/levelup.mid via xxd. CONFIGURE_DEPENDS
# re-globs on every reconfigure, so adding/removing a .mid and re-running
# cmake picks it up; the custom command's own file-level DEPENDS makes sure
# a rebuild is triggered whenever any of those files' *contents* change too.
# Needs `sh` and `xxd` on PATH - on Windows that means Git Bash, MSYS2, or WSL.
file(GLOB_RECURSE MIDI_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/midi/music/*.mid"
    "${CMAKE_SOURCE_DIR}/midi/sfx/*.mid"
)

find_program(SH_EXECUTABLE sh REQUIRED)

set(GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated")
add_custom_command(
    OUTPUT "${GENERATED_DIR}/music_tracks.h" "${GENERATED_DIR}/sfx_levelup_mid.h"
    COMMAND ${SH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/generate_midi_headers.sh"
    DEPENDS ${MIDI_SOURCE_FILES} "${CMAKE_SOURCE_DIR}/generate_midi_headers.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Embedding MIDI assets from midi/ via xxd"
    VERBATIM
)
add_custom_target(midi_headers DEPENDS "${GENERATED_DIR}/music_tracks.h" "${GENERATED_DIR}/sfx_levelup_mid.h")

add_executable(bouncingbabies WIN32 bouncingbabies.cpp dbopl.cpp dbopl_wrapper.cpp instruments.cpp midi_render.cpp)
add_dependencies(bouncingbabies midi_headers)
target_include_directories(bouncingbabies PRIVATE "${GENERATED_DIR}")

target_link_libraries(bouncingbabies PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    SDL2::SDL2
    SDL2_ttf::SDL2_ttf
)
