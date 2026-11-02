# PartiesLauncher.exe: the update supervisor that ships in every build
# directory. The shell lives in a library so the e2e harness can reuse its
# file primitives; the decisions live in parties_update_core.
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)

# Defense in depth behind the signed manifests: when set, every downloaded
# executable must carry a valid Authenticode signature whose signer's common
# name is exactly this (docs/releasing.md). Empty skips the check.
set(PARTIES_AUTHENTICODE_PUBLISHER "" CACHE STRING "Common name of the Authenticode signer every downloaded executable must carry")

add_library(parties_launcher_shell STATIC
    src/app_process.cpp
    src/authenticode.cpp
    src/build_info.cpp
    src/build_store.cpp
    src/data_guard.cpp
    src/file_io.cpp
    src/https_source.cpp
    src/install_plan.cpp
    src/link_server.cpp
    src/local_dir_source.cpp
    src/log.cpp
    src/paths.cpp
    src/recovery_ui.cpp
    src/setup.cpp
    src/setup_payload.cpp
    src/shell_integration.cpp
    src/state_store.cpp
    src/supervisor.cpp
    src/testing.cpp
    src/update_worker.cpp
    ${CMAKE_SOURCE_DIR}/client/src/sqlite_file.cpp
)
target_include_directories(parties_launcher_shell
    PUBLIC src
    PRIVATE ${CMAKE_SOURCE_DIR}/client/include)
target_link_libraries(parties_launcher_shell
    PUBLIC parties_update_core
    PRIVATE parties_update_release_keys rapidjson unofficial::sqlite3::sqlite3 bcrypt comctl32 shell32 winhttp
        wintrust crypt32 ole32 propsys)
target_compile_definitions(parties_launcher_shell PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
if(PARTIES_UPDATE_TESTING)
    target_compile_definitions(parties_launcher_shell PRIVATE PARTIES_UPDATE_TESTING)
endif()
if(PARTIES_AUTHENTICODE_PUBLISHER)
    set_property(SOURCE src/authenticode.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS "PARTIES_AUTHENTICODE_PUBLISHER=\"${PARTIES_AUTHENTICODE_PUBLISHER}\"")
endif()

add_executable(PartiesLauncher WIN32 src/main.cpp)
target_link_libraries(PartiesLauncher PRIVATE parties_launcher_shell)

# PartiesSetup.exe: PartiesLauncher.exe packed with a payload directory
# (manifest.json, Parties.exe and the build's files, see setup_payload.h).
# Not part of "all"; build it with --target PartiesSetup.
set(PARTIES_SETUP_PAYLOAD_DIR "" CACHE PATH
    "Payload directory for PartiesSetup.exe. Empty: testing builds stage this build's own executables.")
set(setup_exe "${CMAKE_CURRENT_BINARY_DIR}/PartiesSetup.exe")
if(PARTIES_SETUP_PAYLOAD_DIR)
    set(setup_payload "${PARTIES_SETUP_PAYLOAD_DIR}")
    set(setup_payload_depends "")
elseif(PARTIES_UPDATE_TESTING AND BUILD_CLIENT)
    set(setup_payload "${CMAKE_CURRENT_BINARY_DIR}/setup-payload")
    set(setup_payload_depends "${setup_payload}/manifest.json")
    # The e2e harness key (src/testing.cpp), which only testing launchers trust.
    set(harness_key f769e71268cc70210831656bcc920641326a5bd43fe4a015881f3515a8f908a8)
    add_custom_command(OUTPUT "${setup_payload}/manifest.json"
        COMMAND ${CMAKE_COMMAND} -DSTUB=$<TARGET_FILE:parties_boot_stub> -DLAUNCHER=$<TARGET_FILE:PartiesLauncher>
            -DCLIENT=$<TARGET_FILE:parties_client> -DTOOL=$<TARGET_FILE:parties_manifest_tool>
            -DKEY_HEX=${harness_key} -DOUT=${setup_payload}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/stage_setup_payload.cmake
        DEPENDS PartiesLauncher parties_boot_stub parties_client parties_manifest_tool
            ${CMAKE_CURRENT_SOURCE_DIR}/stage_setup_payload.cmake
        VERBATIM)
endif()
if(DEFINED setup_payload)
    add_custom_command(OUTPUT "${setup_exe}"
        COMMAND $<TARGET_FILE:PartiesLauncher> --pack=${setup_payload} --out=${setup_exe}
        DEPENDS PartiesLauncher ${setup_payload_depends}
        VERBATIM)
    add_custom_target(PartiesSetup DEPENDS "${setup_exe}")
endif()
