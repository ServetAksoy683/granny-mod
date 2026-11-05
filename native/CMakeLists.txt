cmake_minimum_required(VERSION 3.18)
project(modlib C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(dobby
  GIT_REPOSITORY https://github.com/jmpews/Dobby.git
  GIT_TAG master
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(dobby)

add_library(modlib SHARED src/main.cpp)
target_include_directories(modlib PRIVATE ${dobby_SOURCE_DIR}/include)
target_compile_options(modlib PRIVATE -fvisibility=hidden -O2)
target_link_libraries(modlib dobby log)
set_target_properties(modlib PROPERTIES LINK_FLAGS "-s -Wl,-z,max-page-size=16384")
