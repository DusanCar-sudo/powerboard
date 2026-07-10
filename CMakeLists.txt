cmake_minimum_required(VERSION 3.22)
project(powerboard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")

find_package(ftxui CONFIG REQUIRED)

add_executable(powerboard
    src/main.cpp
)
target_link_libraries(powerboard PRIVATE ftxui::screen ftxui::dom ftxui::component sqlite3)
