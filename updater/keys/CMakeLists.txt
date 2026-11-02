if(NOT TARGET parties_update_core)
    return()
endif()

file(GLOB release_key_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/production/*.pub")
list(SORT release_key_files)

set(RELEASE_KEY_ENTRIES "")
set(release_key_count 0)
foreach(key_file IN LISTS release_key_files)
    file(READ "${key_file}" key_hex)
    string(STRIP "${key_hex}" key_hex)
    string(LENGTH "${key_hex}" key_length)
    if(NOT key_length EQUAL 64 OR NOT key_hex MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "${key_file} must hold one Ed25519 public key as 64 lowercase hex characters")
    endif()
    string(SUBSTRING "${key_hex}" 0 16 key_id)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," key_bytes "${key_hex}")
    string(APPEND RELEASE_KEY_ENTRIES "        {\"${key_id}\", {${key_bytes}}},\n")
    math(EXPR release_key_count "${release_key_count} + 1")
endforeach()

if(release_key_count GREATER 2)
    message(FATAL_ERROR "updater/keys/production holds ${release_key_count} keys; a keyring pins at most two (current and next)")
endif()
if(release_key_count EQUAL 0)
    message(WARNING "No production update key in updater/keys/production. This build pins no "
        "update key, so its launcher verifies no channel manifest and never offers an update. "
        "See \"Set up the signing key the first time\" in docs/releasing.md.")
else()
    message(STATUS "Pinned production update keys: ${release_key_count}")
endif()

configure_file(release_keys.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/parties/update/release_keys.h" @ONLY)

add_library(parties_update_release_keys INTERFACE)
target_include_directories(parties_update_release_keys INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(parties_update_release_keys INTERFACE parties_update_core)
