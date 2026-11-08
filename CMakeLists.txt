cmake_minimum_required(VERSION 3.16)
project(OpenSmoothScroll VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/O2 /GL /W3 /DUNICODE /D_UNICODE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS /LTCG")
elseif(MINGW OR MSYS)
    add_compile_options(-O2 -Wall)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s -mwindows")
endif()

add_executable(OpenSmoothScroll WIN32 src/main.cpp)

target_link_libraries(OpenSmoothScroll PRIVATE
    winmm
    gdi32
    shell32
)
