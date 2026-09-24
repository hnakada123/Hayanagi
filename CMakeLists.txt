cmake_minimum_required(VERSION 3.16)
project(Hayanagi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(hayanagi_tsume STATIC src/position.cpp src/tsume.cpp)
target_include_directories(hayanagi_tsume SYSTEM PUBLIC src)
set_target_properties(hayanagi_tsume PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

add_executable(hayanagi
    src/main.cpp
    src/book.cpp
    src/search.cpp
    src/usi_engine.cpp
)

target_include_directories(hayanagi PRIVATE src)
target_link_libraries(hayanagi PRIVATE hayanagi_tsume Threads::Threads)
set_target_properties(hayanagi PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if(MSVC)
    target_compile_options(hayanagi PRIVATE /W4)
    target_compile_options(hayanagi_tsume PRIVATE /W4)
else()
    target_compile_options(hayanagi PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(hayanagi_tsume PRIVATE -Wall -Wextra -Wpedantic)
endif()
