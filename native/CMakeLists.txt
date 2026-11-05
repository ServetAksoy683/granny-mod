cmake_minimum_required(VERSION 3.18)
project(modlib CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(modlib SHARED src/main.cpp)
target_compile_options(modlib PRIVATE -fvisibility=hidden -O2)
target_link_libraries(modlib log)
set_target_properties(modlib PROPERTIES LINK_FLAGS "-s -Wl,-z,max-page-size=16384")
