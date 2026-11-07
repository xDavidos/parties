#pragma once

// The payload's view of the host that loaded it: where the data lives, the
// link to the launcher, the health report, and the only ways it ends the
// process early.

#include <client/exit_codes.h>
#include <parties/host_api.h>
#include <parties/update/link.h>

#import <Metal/Metal.h>

#include <cstdint>
#include <filesystem>
#include <string>

@class MTKView;

namespace parties::client::macos {

// The app's end of the link, over the socket the host connected. Does
// nothing when the app runs unmanaged.
class LauncherLink {
public:
    explicit LauncherLink(int fd);
    void report_healthy(uint32_t data_schema);
    // Reads what has arrived without blocking. True when exit_requested was
    // among it: the app then quits through its normal path.
    bool poll_exit_requested();

private:
    void send(const update::link::AppMessage& message);
    void closed();

    int fd_;
    update::link::LineReader reader_;
};

// The launcher judges a new build by this report, sent once: ui/lobby.rml is
// loaded, and the window rendered or rendering is paused because the window
// is hidden or minimized. A frame counts as rendered when the screen
// presented it or, where presentedTime is never set (the paravirtual GPU of
// CI runners and VMs), after 60 frames the GPU completed in a visible window.
class HealthReporter {
public:
    explicit HealthReporter(LauncherLink& link) : link_(link) {}
    void lobby_loaded(MTKView* view);
    // Call before the buffer presents the view's drawable and commits.
    void watch_frame(id<MTLCommandBuffer> buffer, MTKView* view);
    void rendering_suspended();

private:
    void drawable_finished(double presented_time, bool window_visible);
    void command_buffer_finished(MTLCommandBufferStatus status);
    void rendered(const char* how);
    void report_if_healthy();
    void report_waiting(MTKView* view);

    LauncherLink& link_;
    bool lobby_loaded_ = false;
    bool rendered_ = false;
    bool reported_ = false;
    uint64_t unpresented_ = 0;
    uint64_t gpu_errors_ = 0;
};

class MacRuntime {
public:
    explicit MacRuntime(const PartiesHostApi& host);

    std::filesystem::path data_dir() const { return host_.data_dir; }
    std::filesystem::path db_path() const { return data_dir() / "parties.db"; }
    std::filesystem::path backups_dir() const { return host_.backups_dir; }
    LauncherLink& link() { return link_; }
    HealthReporter& health() { return health_; }

    // Running without persistence would silently lose everything the user
    // does. Says why, then exits with a code the launcher understands:
    // DataUnavailable is never blamed on the build.
    [[noreturn]] void refuse_to_start(ExitCode code, const std::filesystem::path& where,
                                      const std::string& reason) const;
    [[noreturn]] void init_failed(const char* what) const;

private:
    const PartiesHostApi& host_;
    LauncherLink link_;
    HealthReporter health_;
};

// Valid once parties_client_main has started.
MacRuntime& runtime();

} // namespace parties::client::macos
