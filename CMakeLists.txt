cmake_minimum_required(VERSION 3.16)

# バージョンは src/version.h の HAYANAGI_VERSION が唯一の定義元
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" HAYANAGI_VERSION_LINE REGEX "^#define HAYANAGI_VERSION \"[0-9.]+\"")
string(REGEX REPLACE "^#define HAYANAGI_VERSION \"([0-9.]+)\".*$" "\\1" HAYANAGI_VERSION "${HAYANAGI_VERSION_LINE}")
if(NOT HAYANAGI_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "src/version.h から HAYANAGI_VERSION を読めません: '${HAYANAGI_VERSION_LINE}'")
endif()

project(Hayanagi VERSION ${HAYANAGI_VERSION} LANGUAGES CXX)

# add_subdirectory で組み込む側（ShogiBoardQ など）からもバージョンを参照できるようにする
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(HAYANAGI_VERSION "${HAYANAGI_VERSION}" PARENT_SCOPE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(hayanagi_tsume STATIC src/position.cpp src/tsume.cpp)
target_include_directories(hayanagi_tsume SYSTEM PUBLIC src)
target_link_libraries(hayanagi_tsume PUBLIC Threads::Threads)
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

# 実行ファイルと説明書の配置。定跡同梱物は出典確認付きのpackage_release.pyで作成する。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    install(TARGETS hayanagi RUNTIME DESTINATION . COMPONENT Runtime)
    install(FILES README.md DESTINATION . COMPONENT Runtime)
endif()

# 単体テストは Hayanagi 自体をビルドするときだけ既定で有効にする
# （ShogiBoardQ などから add_subdirectory されたときは無効）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(HAYANAGI_BUILD_TESTS_DEFAULT ON)
else()
    set(HAYANAGI_BUILD_TESTS_DEFAULT OFF)
endif()
option(HAYANAGI_BUILD_TESTS "Hayanagi の単体テストをビルドする" ${HAYANAGI_BUILD_TESTS_DEFAULT})

set(HAYANAGI_TARGETS hayanagi hayanagi_tsume)
option(HAYANAGI_BUILD_BOOK_TOOLS "Build opening-book validation and match tools" ${HAYANAGI_BUILD_TESTS_DEFAULT})
if(HAYANAGI_BUILD_BOOK_TOOLS)
    add_executable(hayanagi_book_positions tools/book_positions.cpp)
    target_link_libraries(hayanagi_book_positions PRIVATE hayanagi_tsume)
    set_target_properties(hayanagi_book_positions PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    list(APPEND HAYANAGI_TARGETS hayanagi_book_positions)
    add_executable(hayanagi_match_referee tools/match_referee.cpp)
    target_link_libraries(hayanagi_match_referee PRIVATE hayanagi_tsume)
    set_target_properties(hayanagi_match_referee PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    list(APPEND HAYANAGI_TARGETS hayanagi_match_referee)
endif()

# ローカルで生成した候補も、公開済み定跡を上書きせずに配置できる。
set(HAYANAGI_BOOK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/book/hayanagi_book.db" CACHE FILEPATH
    "実行ファイル横へコピーする定跡。空文字列ならコピーしない")
# 改名前の既定値を保持したビルドディレクトリも、新しい配置へ移行する。
if(HAYANAGI_BOOK_SOURCE STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/book/standard_book.db"
        AND NOT EXISTS "${HAYANAGI_BOOK_SOURCE}")
    set(HAYANAGI_BOOK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/book/hayanagi_book.db" CACHE FILEPATH
        "実行ファイル横へコピーする定跡。空文字列ならコピーしない" FORCE)
endif()
if(HAYANAGI_BOOK_SOURCE AND EXISTS "${HAYANAGI_BOOK_SOURCE}")
    add_custom_target(hayanagi_book ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:hayanagi>/book"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HAYANAGI_BOOK_SOURCE}"
            "$<TARGET_FILE_DIR:hayanagi>/book/hayanagi_book.db"
        DEPENDS hayanagi "${HAYANAGI_BOOK_SOURCE}"
        VERBATIM)
endif()
if(HAYANAGI_BUILD_TESTS)
    enable_testing()
    add_executable(hayanagi_tests
        tests/test_main.cpp
        tests/test_position.cpp
        tests/test_tsume.cpp
        tests/test_parallel.cpp
        tests/test_search.cpp
        src/search.cpp
    )
    target_link_libraries(hayanagi_tests PRIVATE hayanagi_tsume)
    set_target_properties(hayanagi_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_test(NAME hayanagi_tests COMMAND hayanagi_tests)
    if(HAYANAGI_BUILD_BOOK_TOOLS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME hayanagi_book_tests COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_book.py
                --engine $<TARGET_FILE:hayanagi>
                --validator $<TARGET_FILE:hayanagi_book_positions>)
            add_test(NAME hayanagi_match_book_tests COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_match_book.py
                --engine $<TARGET_FILE:hayanagi>
                --referee $<TARGET_FILE:hayanagi_match_referee>)
            add_test(NAME hayanagi_diagnose_book_tests COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_diagnose_book.py)
            add_test(NAME hayanagi_search_regressions COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_search_regressions.py
                --engine $<TARGET_FILE:hayanagi>
                --referee $<TARGET_FILE:hayanagi_match_referee>)
            add_test(NAME hayanagi_compare_books_tests COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compare_books.py
                --engine $<TARGET_FILE:hayanagi>
                --referee $<TARGET_FILE:hayanagi_match_referee>)
            add_test(NAME hayanagi_package_release_tests COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_release.py
                --engine $<TARGET_FILE:hayanagi>)
        endif()
    endif()
    list(APPEND HAYANAGI_TARGETS hayanagi_tests)
endif()

# Clang の -Wconversion は -Wsign-conversion を含む（GCC は含まない）。
# 盤面や表を int で添字付けする設計なので、符号変換の警告は GCC と同じく無効にする。
foreach(target IN LISTS HAYANAGI_TARGETS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    endif()
endforeach()
