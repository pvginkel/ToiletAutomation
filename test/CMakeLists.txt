# The host tests: the firmware's pure logic, built natively against GoogleTest. They are the
# repository's `kc project test`, after the firmware build (docs/high-level-design.md, section 9):
#
#   cmake -S test -B build-test -G Ninja && cmake --build build-test && ctest --test-dir build-test --output-on-failure

cmake_minimum_required(VERSION 3.20)

project(toilet_automation_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

get_filename_component(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../main ABSOLUTE)

# The pure logic (docs/high-level-design.md, section 4): main/motion/, main/relay/, main/air/ and
# main/config/. main/air/ holds the pure half of the air pipeline: the ESP half is main/board/.
set(PURE_DIRS ${MAIN_DIR}/motion ${MAIN_DIR}/relay ${MAIN_DIR}/air ${MAIN_DIR}/config)

set(PURE_FILES "")
foreach(dir ${PURE_DIRS})
    file(GLOB_RECURSE files CONFIGURE_DEPENDS ${dir}/*.h ${dir}/*.cpp)
    list(APPEND PURE_FILES ${files})
endforeach()

# An include is checked when the build configures, so an edit to a pure file configures again.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PURE_FILES})

# The rule that makes this build possible: the pure logic includes the C++ standard library and
# itself, and nothing else, ESP-IDF least of all. An angle include must be a standard C++ header,
# which has no extension. A quoted include must be a pure file, by its path from the including file
# or from main/.
foreach(file ${PURE_FILES})
    get_filename_component(dir ${file} DIRECTORY)
    file(STRINGS ${file} include_lines REGEX "^[ \t]*#[ \t]*include")

    foreach(line ${include_lines})
        if(line MATCHES "#[ \t]*include[ \t]*<([^>]+)>")
            set(included ${CMAKE_MATCH_1})
            if(included MATCHES "\\.")
                message(FATAL_ERROR "${file}: the pure logic includes the C++ standard library only, not <${included}>")
            endif()
        elseif(line MATCHES "#[ \t]*include[ \t]*\"([^\"]+)\"")
            set(included ${CMAKE_MATCH_1})
            get_filename_component(from_file ${included} ABSOLUTE BASE_DIR ${dir})
            get_filename_component(from_main ${included} ABSOLUTE BASE_DIR ${MAIN_DIR})
            if(NOT from_file IN_LIST PURE_FILES AND NOT from_main IN_LIST PURE_FILES)
                message(FATAL_ERROR "${file}: the pure logic includes the C++ standard library only, not \"${included}\"")
            endif()
        endif()
    endforeach()
endforeach()

# GoogleTest, without gmock.
include(FetchContent)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

# Nothing of ESP-IDF is on this build's include path: main/ is, for the pure files' paths.
set(PURE_SOURCES ${PURE_FILES})
list(FILTER PURE_SOURCES INCLUDE REGEX "\\.cpp$")

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(toilet_automation_tests ${TEST_SOURCES} ${PURE_SOURCES})
target_include_directories(toilet_automation_tests PRIVATE ${MAIN_DIR})
target_link_libraries(toilet_automation_tests PRIVATE GTest::gtest_main)
target_compile_options(toilet_automation_tests PRIVATE -Wall -Wextra -Werror)

enable_testing()

include(GoogleTest)
gtest_discover_tests(toilet_automation_tests)
