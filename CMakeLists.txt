cmake_minimum_required(VERSION 4.0)
project(clirasterizer)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(3rd_party)

add_executable(clirasterizer main.cpp)
target_link_libraries(clirasterizer PRIVATE hmm parallel_util stb tinyobjloader)
add_custom_command(TARGET clirasterizer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ARGS "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${CMAKE_CURRENT_BINARY_DIR}/assets"
    COMMAND_EXPAND_LISTS
)