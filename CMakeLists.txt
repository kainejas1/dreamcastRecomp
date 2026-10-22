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

# The recompiled game: whatever `dcrecomp emit ... --out out/sa2` wrote, plus
# the runtime. Only exists once that output is there (re-run cmake after emit).
set(DC_GAME_DIR ${CMAKE_SOURCE_DIR}/out/sa2 CACHE PATH "dcrecomp emit output to build")
file(GLOB DC_GAME_SRC CONFIGURE_DEPENDS ${DC_GAME_DIR}/*.c)
if(DC_GAME_SRC)
  add_executable(sa2
    runtime/main.c
    runtime/aica.c
    runtime/bios.c
    runtime/coro.c
    runtime/gdrom.c
    runtime/holly.c
    runtime/memory.c
    runtime/sh4_helpers.c
    ${DC_GAME_SRC}
  )
  target_include_directories(sa2 PRIVATE runtime ${DC_GAME_DIR})
  if(NOT MSVC)
    target_link_libraries(sa2 PRIVATE m)
  endif()
  # generated code is huge, optimise it even in debug builds
  set_source_files_properties(${DC_GAME_SRC} PROPERTIES COMPILE_OPTIONS "-O1")
  # -DDC_TRACE=ON: record recently entered functions (slower, for debugging)
  option(DC_TRACE "trace function entries in the recompiled game" OFF)
  if(DC_TRACE)
    target_compile_definitions(sa2 PRIVATE DC_TRACE)
  endif()
endif()
