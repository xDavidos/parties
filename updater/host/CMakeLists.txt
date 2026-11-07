# The macOS host executable and the app bundle around it. Automatic updates
# will never replace the host, so tools/update/check_host.sh caps its size.
set(PARTIES_APPLE_TEAM_ID "" CACHE STRING
    "Team ID whose Developer ID signature retail builds require of every Parties dylib and of a peer on the front door")

add_library(parties_host_api INTERFACE)
target_include_directories(parties_host_api INTERFACE include)

add_executable(parties_host MACOSX_BUNDLE src/host_main.mm src/payload.cpp src/front_door_client.mm)
target_include_directories(parties_host PRIVATE ${CMAKE_SOURCE_DIR}/client/include)
target_compile_definitions(parties_host PRIVATE
    $<$<BOOL:${BUILD_IN_RETAIL}>:PARTIES_RETAIL=1>
    $<$<BOOL:${PARTIES_UPDATE_TESTING}>:PARTIES_UPDATE_TESTING>)
target_link_libraries(parties_host PRIVATE parties_host_api parties_update_link "-framework AppKit")
if(BUILD_IN_RETAIL)
    target_link_libraries(parties_host PRIVATE "-framework Security")
    if(PARTIES_APPLE_TEAM_ID)
        set_property(SOURCE src/payload.cpp APPEND PROPERTY
            COMPILE_DEFINITIONS "PARTIES_APPLE_TEAM_ID=\"${PARTIES_APPLE_TEAM_ID}\"")
    endif()
endif()

set(MACOS_ICON "${CMAKE_SOURCE_DIR}/client/res/AppIcon.icns")
set_source_files_properties(${MACOS_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
target_sources(parties_host PRIVATE ${MACOS_ICON})

set_target_properties(parties_host PROPERTIES
    OUTPUT_NAME Parties
    MACOSX_BUNDLE_BUNDLE_NAME "Parties"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.parties.client"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/client/res/Info.plist.in"
    MACOSX_BUNDLE_ICON_FILE "AppIcon"
    # Use the git-tag-derived version (e.g. 0.0.0.19), NOT PROJECT_VERSION.
    # Sparkle compares the appcast's <sparkle:version> against CFBundleVersion
    # token for token, so it must be the full 4-component tag.
    MACOSX_BUNDLE_BUNDLE_VERSION "${PARTIES_VERSION_STR}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PARTIES_VERSION_STR}"
    MACOSX_BUNDLE_COPYRIGHT "MIT License"
)

# The seed payload, and Sparkle while the payload still links it.
set(frameworks "${CMAKE_CURRENT_BINARY_DIR}/Parties.app/Contents/Frameworks")
add_custom_command(OUTPUT "${frameworks}/PartiesClient.dylib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${frameworks}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:parties_client>" "${frameworks}/PartiesClient.dylib"
    DEPENDS parties_client
)
add_custom_command(OUTPUT "${frameworks}/PartiesLauncher.dylib"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${frameworks}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:PartiesLauncher>" "${frameworks}/PartiesLauncher.dylib"
    DEPENDS PartiesLauncher
)
add_custom_target(parties_host_payload DEPENDS "${frameworks}/PartiesClient.dylib" "${frameworks}/PartiesLauncher.dylib")
add_dependencies(parties_host parties_host_payload)
if(BUILD_IN_RETAIL)
    include(FetchContent)
    FetchContent_GetProperties(sparkle)
    # ditto preserves the framework's symlinks, unlike cmake -E copy_directory.
    add_custom_command(TARGET parties_host POST_BUILD
        COMMAND ditto "${sparkle_SOURCE_DIR}/Sparkle.framework" "${frameworks}/Sparkle.framework"
    )
endif()
