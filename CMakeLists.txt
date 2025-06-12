cmake_minimum_required(VERSION 3.31)
project(TinyRetro)

set(CMAKE_CXX_STANDARD 17)
string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

add_subdirectory(nesemu)
add_subdirectory(gbemu)