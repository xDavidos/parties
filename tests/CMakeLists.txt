find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(toml11 CONFIG REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
if(BUILD_CLIENT)
    find_package(RmlUi CONFIG REQUIRED)
endif()
set_target_properties(toml11::toml11 PROPERTIES INTERFACE_COMPILE_OPTIONS "")

add_executable(parties_integration_test
    integration_test.cpp
    ../server/src/server.cpp
    ../server/src/quic_server.cpp
    ../server/src/database.cpp
    ../server/src/config.cpp
    ../client/src/net_client_msquic.cpp
)

target_include_directories(parties_integration_test
    PRIVATE
        ../server/include
        ../client/include
)

target_link_libraries(parties_integration_test
    PRIVATE
        parties_common
        unofficial::sqlite3::sqlite3
        toml11::toml11
)

if(WIN32)
    target_link_libraries(parties_integration_test PRIVATE ncrypt.lib crypt32.lib)
endif()

# ── Voice under a congested last mile (server + MsQuic + UDP relay) ──────────
add_executable(parties_voice_congestion_test
    voice_congestion_test.cpp
    ../server/src/server.cpp
    ../server/src/quic_server.cpp
    ../server/src/database.cpp
    ../server/src/config.cpp
    ../client/src/net_client_msquic.cpp
)
target_include_directories(parties_voice_congestion_test
    PRIVATE
        ../server/include
        ../client/include
)
target_link_libraries(parties_voice_congestion_test
    PRIVATE
        parties_common
        unofficial::sqlite3::sqlite3
        toml11::toml11
)
if(WIN32)
    target_link_libraries(parties_voice_congestion_test PRIVATE ncrypt.lib crypt32.lib)
endif()

# ── Voice packet-loss resilience (offline, deterministic) ─────────────────────
find_package(Opus CONFIG REQUIRED)

add_executable(parties_voice_loss_test
    voice_loss_test.cpp
    ../client/src/voice_mixer.cpp
)

target_include_directories(parties_voice_loss_test
    PRIVATE
        ../client/include
)

target_link_libraries(parties_voice_loss_test
    PRIVATE
        parties_common
        Opus::opus
)

# ── Secondary/auxiliary audio stream (offline, deterministic) ─────────────────
add_executable(parties_aux_stream_test
    aux_stream_test.cpp
    ../client/src/voice_mixer.cpp
)

target_include_directories(parties_aux_stream_test
    PRIVATE
        ../client/include
)

target_link_libraries(parties_aux_stream_test
    PRIVATE
        parties_common
        Opus::opus
)

# ── RmlUi client tests (offline, deterministic) ─────────────────────────────
if(BUILD_CLIENT)
    add_executable(parties_ios_back_gesture_test ios_back_gesture_test.cpp)
    target_include_directories(parties_ios_back_gesture_test PRIVATE ../client/include)
    add_executable(parties_ios_audio_routes_test ios_audio_routes_test.cpp)
    target_include_directories(parties_ios_audio_routes_test PRIVATE ../client/include)
    add_executable(parties_ui_stylesheet_test ui_stylesheet_test.cpp)
    target_include_directories(parties_ui_stylesheet_test PRIVATE ../client/include)
    target_link_libraries(parties_ui_stylesheet_test PRIVATE RmlUi::RmlUi)
    target_compile_definitions(parties_ui_stylesheet_test PRIVATE
        PARTIES_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../client/ui")

    add_executable(parties_document_router_test document_router_test.cpp)
    target_include_directories(parties_document_router_test PRIVATE ../client/include)
    target_link_libraries(parties_document_router_test PRIVATE RmlUi::RmlUi)

    add_executable(parties_audio_device_selection_model_test
        audio_device_selection_model_test.cpp
        ../client/src/lobby_model.cpp)
    target_include_directories(parties_audio_device_selection_model_test PRIVATE
        ../client/include
        ../common/include)
    target_link_libraries(parties_audio_device_selection_model_test PRIVATE RmlUi::RmlUi)

    add_executable(parties_channel_stream_model_test
        channel_stream_model_test.cpp
        ../client/src/lobby_model.cpp)
    target_include_directories(parties_channel_stream_model_test PRIVATE ../client/include ../common/include)
    target_link_libraries(parties_channel_stream_model_test PRIVATE RmlUi::RmlUi)
    add_test(NAME parties_channel_stream_model COMMAND parties_channel_stream_model_test)

    add_executable(parties_context_render_manager_lifecycle_test
        context_render_manager_lifecycle_test.cpp)
    target_link_libraries(parties_context_render_manager_lifecycle_test PRIVATE RmlUi::RmlUi)
endif()

if(APPLE AND BUILD_CLIENT AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(parties_macos_status_item_test
        macos_status_item_test.mm
        ../client/src/macos/PartiesStatusItemController.mm)
    target_include_directories(parties_macos_status_item_test PRIVATE ../client/src/macos)
    target_link_libraries(parties_macos_status_item_test PRIVATE "-framework AppKit")
    add_test(NAME parties_macos_status_item COMMAND parties_macos_status_item_test)
    set_tests_properties(parties_macos_status_item PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)
endif()

add_executable(parties_server_query_wake_test server_query_wake_test.cpp)
target_include_directories(parties_server_query_wake_test PRIVATE ../client/include)

add_executable(parties_video_decode_gate_test video_decode_gate_test.cpp)
target_include_directories(parties_video_decode_gate_test PRIVATE ../client/include)

add_executable(parties_idle_sleep_inhibitor_test idle_sleep_inhibitor_test.cpp)
target_include_directories(parties_idle_sleep_inhibitor_test PRIVATE ../client/include)

add_executable(parties_video_rate_control_test video_rate_control_test.cpp)
target_include_directories(parties_video_rate_control_test PRIVATE ../encdec/include)

# ── Screen-share transport policy (offline, deterministic) ───────────────────
add_executable(parties_video_send_controller_test
    video_send_controller_test.cpp
    ../common/src/video_send_controller.cpp
)
target_include_directories(parties_video_send_controller_test PRIVATE ../common/include)

add_executable(parties_video_frame_reorder_test
    video_frame_reorder_test.cpp
    ../common/src/video_frame_reorder.cpp
)
target_include_directories(parties_video_frame_reorder_test PRIVATE ../common/include)

add_executable(parties_video_backlog_test video_backlog_test.cpp)
target_include_directories(parties_video_backlog_test PRIVATE ../common/include)

add_executable(parties_hotkey_combo_test hotkey_combo_test.cpp)
target_include_directories(parties_hotkey_combo_test PRIVATE ../client/include)

add_executable(parties_datagram_backlog_test datagram_backlog_test.cpp)
target_include_directories(parties_datagram_backlog_test PRIVATE ../common/include)

add_executable(parties_application_audio_test
    application_audio_test.cpp
    ../client/src/application_audio.cpp
)
target_include_directories(parties_application_audio_test PRIVATE ../client/include)

if(BUILD_CLIENT)
    find_path(SPEEXDSP_INCLUDE_DIRS "speex/speex_echo.h" REQUIRED)
    find_library(SPEEXDSP_LIBRARY speexdsp REQUIRED)

    add_executable(parties_echo_cancellation_test
        echo_cancellation_test.cpp
        ../client/src/echo_canceller.cpp
    )
    target_include_directories(parties_echo_cancellation_test PRIVATE
        ../client/include
        ${SPEEXDSP_INCLUDE_DIRS})
    target_link_libraries(parties_echo_cancellation_test PRIVATE
        parties_common
        ${SPEEXDSP_LIBRARY})
endif()

# ── User data safety: schema migrations, data location, legacy import ────────
if(WIN32 AND BUILD_CLIENT)
    foreach(data_test settings location legacy_import)
        add_executable(parties_data_${data_test}_test
            data_${data_test}_test.cpp
            ../client/src/settings.cpp
            ../client/src/sqlite_file.cpp
            ../client/src/windows/data_location.cpp
            ../client/src/windows/legacy_import.cpp
        )
        target_include_directories(parties_data_${data_test}_test PRIVATE ../client/include)
        target_link_libraries(parties_data_${data_test}_test PRIVATE
            parties_common
            parties_update_data
            unofficial::sqlite3::sqlite3)
        add_test(NAME parties_data_${data_test} COMMAND parties_data_${data_test}_test)
    endforeach()
endif()

if(WIN32 AND BUILD_CLIENT)
    find_path(AMD_AMF_INCLUDE_DIRS "AMF/core/Result.h" REQUIRED)
    add_executable(parties_amf_decode_state_test amf_decode_state_test.cpp)
    target_include_directories(parties_amf_decode_state_test PRIVATE
        ../encdec/src
        ${AMD_AMF_INCLUDE_DIRS})

    add_executable(parties_amf_decoder_hardware_test amf_decoder_hardware_test.cpp)
    target_include_directories(parties_amf_decoder_hardware_test PRIVATE
        ../encdec/src
        ../common/include
        ${AMD_AMF_INCLUDE_DIRS})
    target_link_libraries(parties_amf_decoder_hardware_test PRIVATE
        parties_encdec
        d3d11
        d3d12
        dxgi)

    add_executable(parties_nvdec_d3d12_interop_test nvdec_d3d12_interop_test.cpp)
    target_include_directories(parties_nvdec_d3d12_interop_test PRIVATE
        ../encdec/src
        ../common/include)
    target_link_libraries(parties_nvdec_d3d12_interop_test PRIVATE
        parties_encdec d3d11 d3d12 dxgi)

    add_executable(parties_nvdec_sequence_policy_test nvdec_sequence_policy_test.cpp)
    target_include_directories(parties_nvdec_sequence_policy_test PRIVATE ../encdec/src)

    add_executable(parties_dx12_video_converter_test dx12_video_converter_test.cpp)
    target_include_directories(parties_dx12_video_converter_test PRIVATE ../client/src)
    target_link_libraries(parties_dx12_video_converter_test PRIVATE
        parties_rmlui_dx12_backend d3d12 dxgi)

    add_executable(parties_context_window_lifecycle_test context_window_lifecycle_test.cpp)
    target_include_directories(parties_context_window_lifecycle_test PRIVATE ../client/include)
    target_link_libraries(parties_context_window_lifecycle_test PRIVATE user32)

    add_executable(parties_win32_single_instance_test win32_single_instance_test.cpp)
    target_include_directories(parties_win32_single_instance_test PRIVATE ../client/include)
    target_link_libraries(parties_win32_single_instance_test PRIVATE user32)
    add_test(NAME parties_win32_single_instance COMMAND parties_win32_single_instance_test)
    set_tests_properties(parties_win32_single_instance PROPERTIES TIMEOUT 15)

    add_executable(parties_win32_power_request_test win32_power_request_test.cpp)
    target_include_directories(parties_win32_power_request_test PRIVATE ../client/include)

    add_executable(parties_win32_tray_test
        win32_tray_test.cpp
        ../client/src/windows/win32_tray.cpp)
    target_include_directories(parties_win32_tray_test PRIVATE ../client/include)
    target_link_libraries(parties_win32_tray_test PRIVATE user32 shell32)
    add_test(NAME parties_win32_tray COMMAND parties_win32_tray_test)
    set_tests_properties(parties_win32_tray PROPERTIES TIMEOUT 15)

    add_executable(parties_window_thumbnail_test
        window_thumbnail_test.cpp
        ../client/src/windows/screen_capture.cpp
    )
    target_include_directories(parties_window_thumbnail_test PRIVATE ../client/include)
    target_link_libraries(parties_window_thumbnail_test PRIVATE
        parties_common d3d11 dxgi windowsapp dwmapi gdi32 user32)

    add_executable(parties_screen_capture_lifecycle_test
        screen_capture_lifecycle_test.cpp
        ../client/src/windows/screen_capture.cpp)
    target_include_directories(parties_screen_capture_lifecycle_test PRIVATE ../client/include)
    target_link_libraries(parties_screen_capture_lifecycle_test PRIVATE
        parties_common d3d11 dxgi windowsapp dwmapi gdi32 user32)
    add_test(NAME parties_screen_capture_lifecycle COMMAND parties_screen_capture_lifecycle_test)
    set_tests_properties(parties_screen_capture_lifecycle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

    add_executable(parties_rmlui_dx12_multiwindow_test
        rmlui_dx12_multiwindow_test.cpp)
    target_link_libraries(parties_rmlui_dx12_multiwindow_test PRIVATE
        parties_rmlui_dx12_backend user32)
endif()

add_test(NAME parties_voice_loss COMMAND parties_voice_loss_test)
add_test(NAME parties_aux_stream COMMAND parties_aux_stream_test)
add_test(NAME parties_aux_stream_soak COMMAND parties_aux_stream_test --soak)
set_tests_properties(parties_aux_stream_soak PROPERTIES TIMEOUT 120)
if(BUILD_CLIENT)
    add_test(NAME parties_ios_back_gesture COMMAND parties_ios_back_gesture_test)
    add_test(NAME parties_ios_audio_routes COMMAND parties_ios_audio_routes_test)
    add_test(NAME parties_ui_stylesheet_variables COMMAND parties_ui_stylesheet_test)
    add_test(NAME parties_document_router COMMAND parties_document_router_test)
    add_test(NAME parties_audio_device_selection_model COMMAND parties_audio_device_selection_model_test)
    add_test(NAME parties_context_render_manager_lifecycle COMMAND parties_context_render_manager_lifecycle_test)
endif()
add_test(NAME parties_server_query_wake COMMAND parties_server_query_wake_test)
add_test(NAME parties_video_decode_gate COMMAND parties_video_decode_gate_test)
add_test(NAME parties_idle_sleep_inhibitor COMMAND parties_idle_sleep_inhibitor_test)
add_test(NAME parties_video_rate_control COMMAND parties_video_rate_control_test)
add_test(NAME parties_video_send_controller COMMAND parties_video_send_controller_test)
add_test(NAME parties_video_frame_reorder COMMAND parties_video_frame_reorder_test)
add_test(NAME parties_video_backlog COMMAND parties_video_backlog_test)
add_test(NAME parties_hotkey_combo COMMAND parties_hotkey_combo_test)
add_test(NAME parties_datagram_backlog COMMAND parties_datagram_backlog_test)
# Server + two headless clients over real MsQuic on a loopback port.
add_test(NAME parties_integration COMMAND parties_integration_test)
add_test(NAME parties_voice_congestion COMMAND parties_voice_congestion_test)
set_tests_properties(parties_integration PROPERTIES TIMEOUT 120)
add_test(NAME parties_application_audio_downmix COMMAND parties_application_audio_test)
if(BUILD_CLIENT)
    add_test(NAME parties_echo_cancellation COMMAND parties_echo_cancellation_test)
endif()
if(WIN32 AND BUILD_CLIENT)
    add_test(NAME parties_win32_power_request COMMAND parties_win32_power_request_test)
    set_tests_properties(parties_win32_power_request PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME parties_amf_decode_state COMMAND parties_amf_decode_state_test)
    add_test(NAME parties_amf_decoder_hardware COMMAND parties_amf_decoder_hardware_test)
    set_tests_properties(parties_amf_decoder_hardware PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME parties_nvdec_d3d12_interop COMMAND parties_nvdec_d3d12_interop_test)
    set_tests_properties(parties_nvdec_d3d12_interop PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME parties_nvdec_d3d12_first_frame COMMAND parties_nvdec_d3d12_interop_test 1)
    set_tests_properties(parties_nvdec_d3d12_first_frame PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME parties_nvdec_retained_h264 COMMAND parties_nvdec_d3d12_interop_test 96 retain)
    add_test(NAME parties_nvdec_retained_av1 COMMAND parties_nvdec_d3d12_interop_test 96 retain av1)
    add_test(NAME parties_nvdec_retained_hevc COMMAND parties_nvdec_d3d12_interop_test 96 retain hevc)
    set_tests_properties(parties_nvdec_retained_h264 parties_nvdec_retained_av1
        parties_nvdec_retained_hevc PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME parties_nvdec_sequence_policy COMMAND parties_nvdec_sequence_policy_test)
    add_test(NAME parties_dx12_video_converter COMMAND parties_dx12_video_converter_test)
    set_tests_properties(parties_dx12_video_converter PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME parties_context_window_lifecycle COMMAND parties_context_window_lifecycle_test)
    add_test(NAME parties_window_thumbnail COMMAND parties_window_thumbnail_test)
    add_test(NAME parties_rmlui_dx12_multiwindow COMMAND parties_rmlui_dx12_multiwindow_test)
    set_tests_properties(parties_rmlui_dx12_multiwindow PROPERTIES TIMEOUT 30)
    add_test(NAME parties_rmlui_dx12_resize COMMAND parties_rmlui_dx12_multiwindow_test --resize)
    add_test(NAME parties_rmlui_dx12_resize_msaa COMMAND parties_rmlui_dx12_multiwindow_test --resize --msaa)
    set_tests_properties(parties_rmlui_dx12_resize parties_rmlui_dx12_resize_msaa PROPERTIES TIMEOUT 60)
endif()

# ── Update manager pure core (offline, deterministic) ────────────────────────
if(TARGET parties_update_core)
    foreach(update_test version data_compat manifest probation state_codec policy link retention)
        add_executable(update_${update_test}_test update_${update_test}_test.cpp)
        target_link_libraries(update_${update_test}_test PRIVATE parties_update_core parties_common)
        add_test(NAME update_${update_test} COMMAND update_${update_test}_test)
    endforeach()

    # Links only the boot library, which proves the boot stub's decisions build
    # without parties_common and OpenSSL.
    add_executable(update_boot_test update_boot_test.cpp)
    target_link_libraries(update_boot_test PRIVATE parties_update_boot)
    add_test(NAME update_boot COMMAND update_boot_test)
    if(PARTIES_UPDATE_TESTING)
        # Runs the real Parties.exe against fake launchers in temporary install roots.
        add_executable(update_fake_launcher WIN32 update_fake_launcher.cpp)
        target_link_libraries(update_fake_launcher PRIVATE parties_update_boot)
        add_executable(update_stub_test update_stub_test.cpp)
        target_link_libraries(update_stub_test PRIVATE parties_update_boot bcrypt)
        add_dependencies(update_stub_test parties_boot_stub update_fake_launcher)
        add_test(NAME update_stub COMMAND update_stub_test
            $<TARGET_FILE:parties_boot_stub> $<TARGET_FILE:update_fake_launcher>)
        set_tests_properties(update_stub PROPERTIES TIMEOUT 300)
    endif()

    add_executable(update_release_ops_test update_release_ops_test.cpp)
    target_link_libraries(update_release_ops_test PRIVATE parties_release_ops parties_common)
    add_test(NAME update_release_ops COMMAND update_release_ops_test)
    if(PARTIES_UPDATE_TESTING)
        # The tool's --keyring flag exists only in testing builds.
        add_executable(update_manifest_tool_test update_manifest_tool_test.cpp)
        target_link_libraries(update_manifest_tool_test PRIVATE parties_update_release_keys parties_common)
        add_dependencies(update_manifest_tool_test parties_manifest_tool)
        add_test(NAME update_manifest_tool COMMAND update_manifest_tool_test $<TARGET_FILE:parties_manifest_tool>)
    else()
        add_test(NAME update_manifest_tool_retail_keyring
            COMMAND parties_manifest_tool verify --envelope none --channel beta --keyring .)
        set_tests_properties(update_manifest_tool_retail_keyring PROPERTIES
            PASS_REGULAR_EXPRESSION "unknown option --keyring")
    endif()
    # 100000 seeded random-walk sequences; about two minutes in a Debug build.
    set_tests_properties(update_probation PROPERTIES TIMEOUT 900)

    if(BUILD_CLIENT)
        add_executable(update_view_test update_view_test.cpp
            ../client/src/update_view.cpp ../client/src/lobby_model.cpp)
        target_include_directories(update_view_test PRIVATE ../client/include ../common/include)
        target_link_libraries(update_view_test PRIVATE parties_update_link RmlUi::RmlUi)
        add_test(NAME update_view COMMAND update_view_test)
    endif()
endif()

# â”€â”€ Update launcher end to end (testing builds only) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# Drives the real PartiesLauncher.exe over fake builds in a temp install root.
if(TARGET PartiesLauncher AND PARTIES_UPDATE_TESTING)
    find_package(RapidJSON CONFIG REQUIRED)
    add_executable(update_fake_app WIN32 update_fake_app.cpp ../client/src/windows/shell_identity.cpp)
    target_link_libraries(update_fake_app PRIVATE parties_update_link rapidjson unofficial::sqlite3::sqlite3)

    add_executable(update_e2e_test update_e2e_test.cpp)
    target_include_directories(update_e2e_test PRIVATE ../client/include)
    target_link_libraries(update_e2e_test PRIVATE
        parties_launcher_shell parties_common unofficial::sqlite3::sqlite3)
    add_dependencies(update_e2e_test PartiesLauncher update_fake_app)
    set(update_e2e_args --launcher $<TARGET_FILE:PartiesLauncher> --fake-app $<TARGET_FILE:update_fake_app>)
    add_test(NAME update_e2e COMMAND update_e2e_test ${update_e2e_args})

    add_executable(update_authenticode_test update_authenticode_test.cpp)
    target_link_libraries(update_authenticode_test PRIVATE parties_launcher_shell)
    add_test(NAME update_authenticode COMMAND update_authenticode_test)
    set_tests_properties(update_e2e PROPERTIES TIMEOUT 1800)
    # Needs a GPU: the real client reaches healthy only after presenting a frame.
    if(TARGET parties_client)
        add_test(NAME update_e2e_smoke COMMAND update_e2e_test ${update_e2e_args}
            --real-client $<TARGET_FILE:parties_client>)
        set_tests_properties(update_e2e_smoke PROPERTIES TIMEOUT 300)
    endif()

    # Setup, repair and uninstall over fake builds. The Apps entry, the Start
    # menu and the Recycle Bin are redirected into the test's own places.
    add_executable(update_setup_test update_setup_test.cpp)
    target_include_directories(update_setup_test PRIVATE ../client/include)
    target_link_libraries(update_setup_test PRIVATE
        parties_launcher_shell parties_common unofficial::sqlite3::sqlite3 ole32 propsys shell32)
    add_dependencies(update_setup_test PartiesLauncher parties_boot_stub update_fake_app)
    set(update_setup_args --launcher $<TARGET_FILE:PartiesLauncher> --stub $<TARGET_FILE:parties_boot_stub>
        --fake-app $<TARGET_FILE:update_fake_app>)
    add_test(NAME update_setup COMMAND update_setup_test ${update_setup_args})
    set_tests_properties(update_setup PROPERTIES TIMEOUT 900)
    # Needs a GPU, like update_e2e_smoke.
    if(TARGET parties_client)
        add_test(NAME update_e2e_setup_smoke COMMAND update_setup_test ${update_setup_args}
            --real-client $<TARGET_FILE:parties_client>)
        set_tests_properties(update_e2e_setup_smoke PROPERTIES TIMEOUT 300)
    endif()
endif()
