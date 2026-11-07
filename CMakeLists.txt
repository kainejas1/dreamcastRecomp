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

# The interpreter, for code the analysis missed. Its table of instructions comes
# out of the recompiler, so it does what the recompiled code does
set(DC_GENERATED ${CMAKE_BINARY_DIR}/generated)
add_custom_command(OUTPUT ${DC_GENERATED}/interp_ops.c
  COMMAND dcrecomp interp-ops ${DC_GENERATED}
  DEPENDS dcrecomp
  VERBATIM)
add_library(dcinterp STATIC runtime/interp.c src/sh4.c ${DC_GENERATED}/interp_ops.c)
target_include_directories(dcinterp PRIVATE runtime src)
if(MSVC)
  set_source_files_properties(${DC_GENERATED}/interp_ops.c PROPERTIES COMPILE_OPTIONS "/O1;/bigobj")
else()
  set_source_files_properties(${DC_GENERATED}/interp_ops.c PROPERTIES COMPILE_OPTIONS "-O1;-g0")
endif()

# Tests that need no game data
enable_testing()
find_package(Python3 COMPONENTS Interpreter)
add_executable(test_time tests/test_time.c runtime/clock.c runtime/tmu.c)
target_include_directories(test_time PRIVATE runtime)
add_test(NAME opcode_table COMMAND dcrecomp selftest)
add_test(NAME time COMMAND test_time)
add_executable(test_aica_chan tests/test_aica_chan.c runtime/aica_chan.c)
target_include_directories(test_aica_chan PRIVATE runtime)
add_test(NAME aica_chan COMMAND test_aica_chan)
add_subdirectory(tests/interp)
if(Python3_Interpreter_FOUND)
  add_test(NAME cdi_convert COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/test_cdi_convert.py)
endif()

# The recompiled game: whatever `dcrecomp emit ... --out out/sa2` wrote, plus
# the runtime. Only exists once that output is there (re-run cmake after emit).
set(DC_GAME_DIR ${CMAKE_SOURCE_DIR}/out/sa2 CACHE PATH "dcrecomp emit output to build")
file(GLOB DC_GAME_SRC CONFIGURE_DEPENDS ${DC_GAME_DIR}/*.c)
if(DC_GAME_SRC)
  add_executable(sa2
    runtime/main.c
    runtime/aica.c
    runtime/aica_chan.c
    runtime/arm7.c
    runtime/bios.c
    runtime/clock.c
    runtime/coro.c
    runtime/gdrom.c
    runtime/holly.c
    runtime/maple.c
    runtime/memory.c
    runtime/patches.c
    runtime/pvr.c
    runtime/sh4_helpers.c
    runtime/tmu.c
    runtime/video.c
    ${DC_GAME_SRC}
  )
  target_include_directories(sa2 PRIVATE runtime ${DC_GAME_DIR})
  target_link_libraries(sa2 PRIVATE dcinterp)
  if(NOT MSVC)
    target_link_libraries(sa2 PRIVATE m)
  endif()
  # generated code is huge: optimise it even in debug builds, and no debug info
  # (with it the exe was 500+ MB and every link slow)
  set_source_files_properties(${DC_GAME_SRC} PROPERTIES COMPILE_OPTIONS "-O1;-g0")

  # SDL3 for the window (and later input and audio), downloaded at configure
  # time and linked statically
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
  FetchContent_MakeAvailable(SDL3)
  target_link_libraries(sa2 PRIVATE SDL3::SDL3-static)

  # The GPU renderer (runtime/gpu.c): SDL_gpu on Vulkan, with its shaders
  # compiled to SPIR-V by glslc from the Vulkan SDK and built into the exe.
  # Without glslc the game still builds, drawing in software only.
  file(GLOB DC_VULKAN_BINS "$ENV{VULKAN_SDK}/Bin" "C:/VulkanSDK/*/Bin")
  find_program(GLSLC glslc HINTS ${DC_VULKAN_BINS})
  if(GLSLC)
    set(DC_SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${DC_SHADER_OUT})
    set(DC_SHADER_HEADERS)
    foreach(stage vert frag)
      set(src ${CMAKE_SOURCE_DIR}/runtime/shaders/pvr.${stage})
      add_custom_command(
        OUTPUT ${DC_SHADER_OUT}/pvr_${stage}.h
        COMMAND ${GLSLC} -O ${src} -o ${DC_SHADER_OUT}/pvr_${stage}.spv
        COMMAND ${CMAKE_COMMAND} -DIN=${DC_SHADER_OUT}/pvr_${stage}.spv -DOUT=${DC_SHADER_OUT}/pvr_${stage}.h
                -DNAME=pvr_${stage}_spv -P ${CMAKE_SOURCE_DIR}/tools/embed.cmake
        DEPENDS ${src} ${CMAKE_SOURCE_DIR}/tools/embed.cmake
        COMMENT "glslc pvr.${stage}")
      list(APPEND DC_SHADER_HEADERS ${DC_SHADER_OUT}/pvr_${stage}.h)
    endforeach()
    target_sources(sa2 PRIVATE runtime/gpu.c ${DC_SHADER_HEADERS})
    target_include_directories(sa2 PRIVATE ${DC_SHADER_OUT})
    target_compile_definitions(sa2 PRIVATE DC_GPU)
    message(STATUS "GPU renderer: on (${GLSLC})")
  else()
    message(STATUS "GPU renderer: off (no glslc; install the Vulkan SDK for it)")
  endif()
  # -DDC_TRACE=ON: record recently entered functions (slower, for debugging)
  option(DC_TRACE "trace function entries in the recompiled game" OFF)
  if(DC_TRACE)
    target_compile_definitions(sa2 PRIVATE DC_TRACE)
  endif()

  # -DDC_GAME_DIR=<repo>/tests/fakegame: a synthetic game that runs the runtime
  # end to end without any disc data
  if(DC_GAME_DIR MATCHES "tests/fakegame/?$")
    add_test(NAME fakegame COMMAND ${CMAKE_COMMAND} -DSA2=$<TARGET_FILE:sa2>
      -DOUT=${CMAKE_BINARY_DIR}/fakegame -P ${CMAKE_SOURCE_DIR}/tests/fakegame.cmake)
  endif()
endif()
