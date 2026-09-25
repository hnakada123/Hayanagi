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

# 単体テストは Hayanagi 自体をビルドするときだけ既定で有効にする
# （ShogiBoardQ などから add_subdirectory されたときは無効）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(HAYANAGI_BUILD_TESTS_DEFAULT ON)
else()
    set(HAYANAGI_BUILD_TESTS_DEFAULT OFF)
endif()
option(HAYANAGI_BUILD_TESTS "Hayanagi の単体テストをビルドする" ${HAYANAGI_BUILD_TESTS_DEFAULT})

set(HAYANAGI_TARGETS hayanagi hayanagi_tsume)
if(HAYANAGI_BUILD_TESTS)
    enable_testing()
    add_executable(hayanagi_tests
        tests/test_main.cpp
        tests/test_position.cpp
        tests/test_tsume.cpp
    )
    target_link_libraries(hayanagi_tests PRIVATE hayanagi_tsume)
    set_target_properties(hayanagi_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_test(NAME hayanagi_tests COMMAND hayanagi_tests)
    list(APPEND HAYANAGI_TARGETS hayanagi_tests)
endif()

foreach(target IN LISTS HAYANAGI_TARGETS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    endif()
endforeach()
