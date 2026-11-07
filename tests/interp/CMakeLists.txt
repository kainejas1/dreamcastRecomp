# Real SH-4 code through the whole pipeline, with no game data: assemble a test
# program, recompile it, run it recompiled and interpreted, then feed the misses
# back through the recompiler and check they're gone.

if(NOT Python3_Interpreter_FOUND)
  message(STATUS "no Python: skipping the interpreter tests")
  return()
endif()

set(work ${CMAKE_BINARY_DIR}/interp_test)
set(prog ${work}/prog.bin)
set(expected ${work}/expected.txt)
set(misses ${work}/misses.txt)

add_custom_command(OUTPUT ${prog} ${expected}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${work}
  COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/prog.py ${prog} ${expected}
  DEPENDS prog.py
  VERBATIM)

function(interp_harness name)
  cmake_parse_arguments(ARG "" "" "EMIT_ARGS;DEPENDS" ${ARGN})
  set(out ${work}/${name})
  set(generated ${out}/funcs_000.c ${out}/lookup.c ${out}/modules.c)
  add_custom_command(OUTPUT ${generated}
    COMMAND dcrecomp emit ${prog} --bin --out ${out} ${ARG_EMIT_ARGS}
    DEPENDS dcrecomp ${prog} ${ARG_DEPENDS}
    VERBATIM)
  add_executable(${name} harness.c ${PROJECT_SOURCE_DIR}/runtime/sh4_helpers.c ${generated})
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/runtime ${out})
  target_link_libraries(${name} PRIVATE dcinterp)
  if(NOT MSVC)
    target_link_libraries(${name} PRIVATE m)
  endif()
endfunction()

# first pass: the analysis can't see `mid`, so it runs in the interpreter
interp_harness(interp_first)

add_custom_command(OUTPUT ${misses}
  COMMAND interp_first ${prog} ${expected} --misses-out ${misses}
  DEPENDS interp_first ${prog} ${expected}
  VERBATIM)

# second pass: told about it, the recompiler makes it a function
interp_harness(interp_second EMIT_ARGS --roots ${misses} DEPENDS ${misses})

add_test(NAME interp_recompiled_vs_interpreted
  COMMAND interp_first ${prog} ${expected} --expect-misses 2)
add_test(NAME interp_misses_fed_back
  COMMAND interp_second ${prog} ${expected} --expect-misses 0)
