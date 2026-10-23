cmake_minimum_required(VERSION 3.16)
project(rsv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# FTXUI's release archive: one small download, checked by its hash.
# A git clone of its whole history could hang for minutes on the login nodes.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  ftxui
  URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.3.tar.gz
  URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88
  TIMEOUT 120
)
FetchContent_MakeAvailable(ftxui)

find_package(Threads REQUIRED)

add_executable(rsv src/main.cpp)
target_link_libraries(rsv PRIVATE ftxui::screen ftxui::dom ftxui::component Threads::Threads)

if(NOT MSVC)
  target_compile_options(rsv PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
  # Optional: -DSTATIC_BUILD=ON for a binary that runs anywhere (needs the static C++ libraries)
  if(STATIC_BUILD)
    target_link_options(rsv PRIVATE -static)
  endif()
endif()

install(TARGETS rsv RUNTIME DESTINATION bin)

# Parser tests on recorded Slurm output: cmake -DRSV_TESTS=ON, then ctest
option(RSV_TESTS "Build the tests" OFF)
if(RSV_TESTS)
  enable_testing()
  add_executable(parse_test tests/parse_test.cpp)
  target_include_directories(parse_test PRIVATE src)
  target_link_libraries(parse_test PRIVATE ftxui::screen ftxui::dom ftxui::component)
  target_compile_options(parse_test PRIVATE -Wall -Wextra)
  add_test(NAME parse_test COMMAND parse_test)
endif()
