# The release pipeline's manifest tool. Windows-only like the rest of the
# update core it links.
if(NOT TARGET parties_update_core)
    return()
endif()

find_package(RapidJSON CONFIG REQUIRED)

add_library(parties_release_ops STATIC release_ops.cpp)
target_include_directories(parties_release_ops PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(parties_release_ops PUBLIC parties_update_core PRIVATE rapidjson)

add_executable(parties_manifest_tool manifest_tool.cpp)
target_link_libraries(parties_manifest_tool PRIVATE
    parties_release_ops parties_update_release_keys parties_common winhttp)
target_compile_definitions(parties_manifest_tool PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN
    $<$<BOOL:${PARTIES_UPDATE_TESTING}>:PARTIES_UPDATE_TESTING>)
