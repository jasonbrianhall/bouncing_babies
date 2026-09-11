cmake_minimum_required(VERSION 3.15)
project(BouncingBabies CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)

add_executable(bouncingbabies WIN32 bouncingbabies.cpp dbopl.cpp  dbopl_wrapper.cpp  instruments.cpp  midi_render.cpp)


target_link_libraries(bouncingbabies PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    SDL2::SDL2
    SDL2_ttf::SDL2_ttf
)
