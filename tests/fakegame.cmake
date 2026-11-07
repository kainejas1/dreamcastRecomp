# Runs the synthetic game (tests/fakegame) through the whole runtime:
#   cmake -DSA2=<exe> -DOUT=<dir> -P tests/fakegame.cmake
#
# - in real time, where the interrupt rates have to come out right
# - twice in virtual time, where the state hashes have to be identical
# - once with a different frame length, where they have to differ, so a match
#   above means something
# - with DC_MISSLOG, which has to list the code nothing was recompiled for
# - with DC_NOINTERP, which has to stop at that code
#
# Each run has a time limit: a game that hangs is a failure too.

file(MAKE_DIRECTORY ${OUT})
file(WRITE ${OUT}/empty.dat "x")   # the runtime insists on a program file

function(run name)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env DC_NOWINDOW=1 DC_STATEEVERY=10 DC_STATEHASH=${OUT}/${name}.txt
                ${ARGN} ${SA2} ${OUT}/empty.dat
        TIMEOUT 30
        RESULT_VARIABLE rc
        ERROR_VARIABLE log)
    message(STATUS "${name}:\n${log}")
    set(rc "${rc}" PARENT_SCOPE)
    set(log "${log}" PARENT_SCOPE)
endfunction()

function(run_ok name)
    run(${name} ${ARGN})
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${name}: exit code ${rc}")
    endif()
endfunction()

run_ok(host)
run_ok(virtual_a DC_VCLOCK=200000)
run_ok(virtual_b DC_VCLOCK=200000)
run_ok(virtual_c DC_VCLOCK=200001)

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/virtual_a.txt ${OUT}/virtual_b.txt
    RESULT_VARIABLE differs)
if(differs)
    message(FATAL_ERROR "two runs with the same DC_VCLOCK gave different state hashes")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files ${OUT}/virtual_a.txt ${OUT}/virtual_c.txt
    RESULT_VARIABLE differs)
if(NOT differs)
    message(FATAL_ERROR "different DC_VCLOCK gave the same state hashes: the hashes aren't seeing anything")
endif()

file(STRINGS ${OUT}/virtual_a.txt lines)
list(LENGTH lines n)
if(n LESS 10)
    message(FATAL_ERROR "only ${n} lines of state hashes")
endif()
message(STATUS "${n} lines of state hashes match")

run_ok(with_misslog DC_VCLOCK=200000 DC_MISSLOG=${OUT}/misses.txt)
file(STRINGS ${OUT}/misses.txt lines)
list(LENGTH lines n)
if(NOT n EQUAL 2)
    message(FATAL_ERROR "expected 2 lines in the miss log, got ${n}: ${lines}")
endif()
list(GET lines 0 first)
list(GET lines 1 second)
if(NOT first MATCHES "^8c200000 main call" OR NOT second MATCHES "^8c200100 main call")
    message(FATAL_ERROR "wrong miss log: ${lines}")
endif()
message(STATUS "miss log is right")

run(no_interp DC_VCLOCK=200000 DC_NOINTERP=1)
if(rc EQUAL 0 OR NOT log MATCHES "no recompiled function there")
    message(FATAL_ERROR "DC_NOINTERP should have stopped at the miss (exit code ${rc})")
endif()
message(STATUS "DC_NOINTERP stops at a miss")
