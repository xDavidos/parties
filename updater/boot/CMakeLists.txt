# Parties.exe, the boot stub. Automatic updates never replace it, so it links
# only what tools/update/check_boot_stub.ps1 allows.
if(NOT WIN32)
    return()
endif()

# Drop CMake's default Windows libraries so the link line shows exactly what the stub uses.
set(CMAKE_CXX_STANDARD_LIBRARIES "")

add_executable(parties_boot_stub WIN32 main.cpp Parties.rc)
set_target_properties(parties_boot_stub PROPERTIES OUTPUT_NAME Parties)
target_compile_definitions(parties_boot_stub PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00
    $<$<BOOL:${PARTIES_UPDATE_TESTING}>:PARTIES_UPDATE_TESTING>)
target_link_libraries(parties_boot_stub PRIVATE parties_update_boot kernel32 user32 comctl32 shell32 bcrypt)
# The manifest is embedded by Parties.rc.
target_link_options(parties_boot_stub PRIVATE /MANIFEST:NO)
