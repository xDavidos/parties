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
    src/link_server.cpp
    src/local_dir_source.cpp
    src/log.cpp
    src/recovery_ui.cpp
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
        wintrust crypt32)
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
