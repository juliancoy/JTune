cmake_minimum_required(VERSION 3.16)
project(JTuneAutotune LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_PYTHON OFF CACHE BOOL "" FORCE)

add_subdirectory(vendor/Loiacono/rtaudio)

add_executable(jtune_autotune
    src/main.cpp
)

target_include_directories(jtune_autotune PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/Loiacono/rtaudio
)

target_link_libraries(jtune_autotune PRIVATE rtaudio)
