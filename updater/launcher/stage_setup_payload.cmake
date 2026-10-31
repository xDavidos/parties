# Stages a setup payload from this build's own executables, signed with the
# e2e harness key that testing launchers pin. Never used for a release: CI
# packs the signed release executables with the channel manifest instead
# (docs/releasing.md).
# cmake -DSTUB= -DLAUNCHER= -DCLIENT= -DTOOL= -DKEY_HEX= -DOUT= -P stage_setup_payload.cmake
foreach(input STUB LAUNCHER CLIENT TOOL KEY_HEX OUT)
    if(NOT DEFINED ${input})
        message(FATAL_ERROR "stage_setup_payload.cmake needs -D${input}=")
    endif()
endforeach()

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${ARGV0} failed: ${result}")
    endif()
endfunction()

set(work "${OUT}.work")
file(REMOVE_RECURSE "${OUT}" "${work}")
file(MAKE_DIRECTORY "${OUT}" "${work}")
file(COPY_FILE "${STUB}" "${OUT}/Parties.exe")
file(COPY_FILE "${LAUNCHER}" "${OUT}/PartiesLauncher.exe")
file(COPY_FILE "${CLIENT}" "${OUT}/parties_client.exe")

run("${CLIENT}" --write-build-info "${work}/build-info.json")
run("${TOOL}" entry --build-info "${work}/build-info.json"
    --file "PartiesLauncher.exe=${OUT}/PartiesLauncher.exe" --file "parties_client.exe=${OUT}/parties_client.exe"
    --url-base "https://example.invalid/" --out "${work}/entry.json")
run("${TOOL}" merge --base none --entry "${work}/entry.json" --channel beta --serial 1 --expires-days 45 --keep 5
    --out "${work}/payload.json")
set(ENV{PARTIES_SETUP_HARNESS_KEY} "${KEY_HEX}")
run("${TOOL}" sign --payload "${work}/payload.json" --key-env PARTIES_SETUP_HARNESS_KEY --out "${OUT}/manifest.json")
file(REMOVE_RECURSE "${work}")
