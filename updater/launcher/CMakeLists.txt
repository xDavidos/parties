# What both launchers do around the update core: SupervisorCore drives step()
# through a platform Executor, StateStore owns the state files, DataGuard the
# user's database and backups, and UpdateWorker the feed. Each platform defines
# the calls in durable_file.h, log.h and test_hooks.h (end_process).
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library(parties_launcher_core STATIC
    src/core_shell/data_guard.cpp
    src/core_shell/install_plan.cpp
    src/core_shell/local_dir_source.cpp
    src/core_shell/recovery_choice.cpp
    src/core_shell/state_store.cpp
    src/core_shell/supervisor_core.cpp
    src/core_shell/test_hooks.cpp
    src/core_shell/update_worker.cpp
    ${CMAKE_SOURCE_DIR}/client/src/sqlite_file.cpp
)
target_include_directories(parties_launcher_core
    PUBLIC src/core_shell
    PRIVATE ${CMAKE_SOURCE_DIR}/client/include)
target_link_libraries(parties_launcher_core
    PUBLIC parties_update_core
    PRIVATE parties_update_release_keys unofficial::sqlite3::sqlite3)
if(PARTIES_UPDATE_TESTING)
    target_compile_definitions(parties_launcher_core PRIVATE PARTIES_UPDATE_TESTING)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # The macOS launcher. The host's shared headers (data_lock.h,
    # front_door_socket.h, macos_layout.h) come with parties_host_api.
    add_library(parties_launcher_macos STATIC
        src/macos/build_store.cpp
        src/macos/code_signing.cpp
        src/macos/file_io.cpp
        src/macos/front_door.mm
        src/macos/install_seed.cpp
        src/macos/log.cpp
        src/macos/supervisor.cpp
    )
    target_include_directories(parties_launcher_macos PUBLIC src/macos)
    target_link_libraries(parties_launcher_macos
        PUBLIC parties_launcher_core parties_host_api
        PRIVATE "-framework AppKit" "-framework Security")
    # Like the host, so both name the same data root (macos_layout.h).
    target_compile_definitions(parties_launcher_macos PUBLIC $<$<BOOL:${BUILD_IN_RETAIL}>:PARTIES_RETAIL=1>)
    if(PARTIES_APPLE_TEAM_ID)
        set_property(SOURCE src/macos/code_signing.cpp APPEND PROPERTY
            COMPILE_DEFINITIONS "PARTIES_APPLE_TEAM_ID=\"${PARTIES_APPLE_TEAM_ID}\"")
    endif()

    # What the host loads in a process it started with --role=launcher.
    add_library(PartiesLauncher SHARED src/macos/launcher_main.mm)
    target_link_libraries(PartiesLauncher PRIVATE parties_launcher_macos "-framework Foundation")
    set_target_properties(PartiesLauncher PROPERTIES PREFIX "" SUFFIX ".dylib")
    return()
endif()

find_package(RapidJSON CONFIG REQUIRED)

set(PARTIES_AUTHENTICODE_PUBLISHER "" CACHE STRING "Common name of the Authenticode signer every downloaded executable must carry")

add_library(parties_launcher_shell STATIC
    src/app_process.cpp
    src/authenticode.cpp
    src/build_info.cpp
    src/build_store.cpp
    src/file_io.cpp
    src/https_source.cpp
    src/legacy_bridge.cpp
    src/link_server.cpp
    src/log.cpp
    src/paths.cpp
    src/recovery_ui.cpp
    src/setup.cpp
    src/setup_payload.cpp
    src/shell_integration.cpp
    src/supervisor.cpp
    src/testing.cpp
    ${CMAKE_SOURCE_DIR}/client/src/windows/legacy_import.cpp
)
target_include_directories(parties_launcher_shell
    PUBLIC src
    PRIVATE ${CMAKE_SOURCE_DIR}/client/include)
target_link_libraries(parties_launcher_shell
    PUBLIC parties_launcher_core
    PRIVATE parties_update_release_keys parties_common rapidjson unofficial::sqlite3::sqlite3 bcrypt comctl32
        shell32 winhttp wintrust crypt32 ole32 propsys)
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

set(PARTIES_SETUP_PAYLOAD_DIR "" CACHE PATH
    "Payload directory for PartiesSetup.exe. Empty: testing builds stage this build's own executables.")
set(setup_exe "${CMAKE_CURRENT_BINARY_DIR}/PartiesSetup.exe")
if(PARTIES_SETUP_PAYLOAD_DIR)
    set(setup_payload "${PARTIES_SETUP_PAYLOAD_DIR}")
    set(setup_payload_depends "")
elseif(PARTIES_UPDATE_TESTING AND BUILD_CLIENT)
    set(setup_payload "${CMAKE_CURRENT_BINARY_DIR}/setup-payload")
    set(setup_payload_depends "${setup_payload}/manifest.json")
    # The e2e harness signing seed (tests/update_e2e_test.cpp); only testing launchers pin its public key (src/testing.cpp).
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
