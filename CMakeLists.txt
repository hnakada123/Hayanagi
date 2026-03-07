cmake_minimum_required(VERSION 3.16)
project(Hayanagi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(hayanagi
    src/main.cpp
    src/book.cpp
    src/position.cpp
    src/search.cpp
    src/usi_engine.cpp
)

target_include_directories(hayanagi PRIVATE src)

if(MSVC)
    target_compile_options(hayanagi PRIVATE /W4)
else()
    target_compile_options(hayanagi PRIVATE -Wall -Wextra -Wpedantic)
endif()
