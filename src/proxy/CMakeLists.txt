cmake_minimum_required(VERSION 3.20)
project(bully_d3d9_proxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(d3d9proxy SHARED proxy.cpp)
target_link_libraries(d3d9proxy PRIVATE d3d12 dxgi)

set_target_properties(d3d9proxy PROPERTIES
    OUTPUT_NAME "d3d9"
    PREFIX ""
)

# Exact export names via .def; x86 only (game is 32-bit).
target_link_options(d3d9proxy PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/exports.def")
target_compile_definitions(d3d9proxy PRIVATE NOMINMAX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "This proxy MUST be built as Win32 (x86): use -A Win32")
endif()
