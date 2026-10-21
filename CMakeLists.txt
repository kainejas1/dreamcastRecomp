cmake_minimum_required(VERSION 3.16)
project(dcrecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(dcrecomp
  src/main.c
  src/analysis.c
  src/emit.c
  src/loader.c
  src/sh4.c
  src/selftest.c
)
