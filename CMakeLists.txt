cmake_minimum_required(VERSION 3.14)
project(rsv)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch FTXUI automatically
include(FetchContent)
FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
    GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

set(SRC
    src/main.cpp
)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)

set(GENERATED_HEADER_NAME defaults)

add_executable(rsv ${SRC})

# Compiler options (GCC/Clang only, skip on MSVC)
if(NOT MSVC)
    target_compile_options(rsv PRIVATE -Wall -Wunused-variable -Wimplicit-fallthrough -Wextra -Wpedantic -O3)
    target_link_options(rsv PRIVATE -static -O3)
endif()

target_link_libraries(rsv
    ftxui::screen
    ftxui::dom
    ftxui::component
)