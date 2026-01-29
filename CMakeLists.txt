cmake_minimum_required(VERSION 3.20)
project(rj_span LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(rj_span WIN32
    src/rj_span.cpp
)

target_compile_definitions(rj_span PRIVATE
    UNICODE
    _UNICODE
)

# Link against the required Windows graphics libraries.
target_link_libraries(rj_span PRIVATE
    d3d11
    dxgi
    d3dcompiler
    windowsapp
)
