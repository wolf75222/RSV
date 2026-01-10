cmake_minimum_required(VERSION 3.14)
project(rsv)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ftxui REQUIRED)

set(SRC
    src/main.cpp
)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)

set(GENERATED_HEADER_NAME defaults)

add_executable(rsv ${SRC})

target_compile_options(rsv PRIVATE -static -Wall -Wunused-variable -Wimplicit-fallthrough -Wextra -Wpedantic -O3)
target_link_options(rsv PRIVATE -static -Wunused-variable -Wall -Wimplicit-fallthrough -Wextra -Wpedantic -O3)

target_link_libraries(rsv
    ftxui::screen
    ftxui::dom
    ftxui::component
)