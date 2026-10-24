cmake_minimum_required(VERSION 3.20)
project(FH2NoPause VERSION 1.0.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The FH2 mod platform (git submodule), only for the fh2mod.h header
add_subdirectory(platform)

add_library(FH2NoPause SHARED src/nopause.cpp)
target_link_libraries(FH2NoPause PRIVATE fh2mod_api user32)
set_target_properties(FH2NoPause PROPERTIES OUTPUT_NAME FH2NoPause PREFIX "")
target_compile_definitions(FH2NoPause PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
    target_compile_options(FH2NoPause PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()
