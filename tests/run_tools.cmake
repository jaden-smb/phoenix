# tests/run_tools.cmake — CLI smoke for the asset-pipeline tool binaries (ctest `tools_cli`).
# Runs the four real converters + the phxpack assembler over the source fixtures that
# pipeline_test dropped into ${WD}/build, asserting each exits 0 and a non-trivial merged bundle
# results. (pipeline_test already verifies CORRECTNESS in-process; this proves the EXECUTABLES
# parse args, link, and run.) Invoked via `cmake -P` with the tool paths passed as -D defines.

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc WORKING_DIRECTORY ${WD})
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "tool failed (exit ${rc}): ${ARGN}")
  endif()
endfunction()

run(${PHXSPRITE} --out build/c_hero.phxspr   --name hero  build/p_hero.sprdef)
run(${PHXTILE}   --out build/c_level.phxtmap  --name level build/p_level.tmj)
run(${PHXSND}    --out build/c_tone.phxsnd    --name tone  build/p_tone.wav)
run(${PHXBIN}    --out build/c_items.phxbin   --name items --header build/c_items.gen.h build/p_items.json)
run(${PHXPACK}   --out build/c_assets.phxp
    build/c_hero.phxspr build/c_level.phxtmap build/c_tone.phxsnd build/c_items.phxbin)

if(NOT EXISTS ${WD}/build/c_assets.phxp)
  message(FATAL_ERROR "phxpack produced no bundle")
endif()
file(SIZE ${WD}/build/c_assets.phxp _sz)
if(_sz LESS 64)
  message(FATAL_ERROR "merged bundle suspiciously small: ${_sz} bytes")
endif()
if(NOT EXISTS ${WD}/build/c_items.gen.h)
  message(FATAL_ERROR "phxbin did not emit the generated header")
endif()
message(STATUS "TOOLS_CLI PASS — merged ${_sz}-byte bundle from 4 converter binaries")
