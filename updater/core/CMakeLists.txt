find_package(RapidJSON CONFIG REQUIRED)

# The boot stub links only this library, so it must never depend on
# parties_common (OpenSSL) or anything else beyond the C++ library and RapidJSON.
add_library(parties_update_boot STATIC
    src/sha256_digest.cpp
)
target_include_directories(parties_update_boot PUBLIC include)
target_link_libraries(parties_update_boot PRIVATE rapidjson)

add_library(parties_update_core STATIC
    src/version.cpp
    src/encoding.cpp
    src/manifest.cpp
)
target_include_directories(parties_update_core PUBLIC include)
target_link_libraries(parties_update_core
    PUBLIC parties_update_boot
    PRIVATE parties_common rapidjson)
