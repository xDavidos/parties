# The data compatibility rule is header-only and shared by every client
# platform, because Settings opens the same database everywhere.
add_library(parties_update_data INTERFACE)
target_include_directories(parties_update_data INTERFACE include)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    return()
endif()

find_package(RapidJSON CONFIG REQUIRED)

# The boot stub links only this library, so it must never depend on
# parties_common (OpenSSL) or anything else beyond the C++ library and RapidJSON.
add_library(parties_update_boot STATIC
    src/sha256_digest.cpp
    src/boot.cpp
)
target_include_directories(parties_update_boot PUBLIC include)
target_link_libraries(parties_update_boot PRIVATE rapidjson)

# The client links the launcher <-> app link codec, and the macOS host the
# front-door codec, so both stay free of OpenSSL like the boot library.
add_library(parties_update_link STATIC
    src/encoding.cpp
    src/version.cpp
    src/channel.cpp
    src/link.cpp
    src/front_door.cpp
)
target_include_directories(parties_update_link PUBLIC include)
target_link_libraries(parties_update_link
    PUBLIC parties_update_boot
    PRIVATE rapidjson)

add_library(parties_update_core STATIC
    src/manifest.cpp
    src/supervisor_state.cpp
    src/probation.cpp
    src/state_codec.cpp
    src/update_policy.cpp
    src/retention.cpp
    src/front_door_policy.cpp
)
target_include_directories(parties_update_core PUBLIC include)
target_link_libraries(parties_update_core
    PUBLIC parties_update_link
    PRIVATE parties_common rapidjson)
