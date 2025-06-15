cmake_minimum_required(VERSION 3.31)
project(TinyRetro)

set(CMAKE_CXX_STANDARD 17)
string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-") # no rtti
# Disable warning about STL and compiler-generated types using noexcept when exceptions are disabled
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4577")

add_subdirectory(nesemu)
add_subdirectory(gbemu)