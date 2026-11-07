#include "mac_runtime.h"

#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include <client/settings_schema.h>
#include <parties/version.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <variant>

namespace parties::client::macos {

namespace {

// Testing builds print the milestones that tools/update/host_payload_probe.sh
// and tools/update/front_door_e2e.sh assert, at the time of day NSLog prints.
#ifdef PARTIES_UPDATE_TESTING
constexpr bool kUpdateTesting = true;
#else
constexpr bool kUpdateTesting = false;
#endif

std::string time_of_day() {
    timeval now{};
    gettimeofday(&now, nullptr);
    tm local{};
    localtime_r(&now.tv_sec, &local);
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                  int(now.tv_usec / 1000));
    return text;
}

} // namespace

LauncherLink::LauncherLink(int fd) : fd_(fd) {
    if (fd_ < 0) return;
    NSLog(@"[Parties] Supervised by the Parties launcher");
    send(update::link::Hello{update::kLinkProtocol, update::kMinLauncherLink,
                             update::Version::parse(parties::APP_VERSION).value_or(update::Version{})});
}

void LauncherLink::closed() {
    NSLog(@"[Parties] The launcher link closed");
    close(fd_);
    fd_ = -1;
}

void LauncherLink::send(const update::link::AppMessage& message) {
    if (fd_ < 0) return;
    const std::string line = update::link::write(message);
    for (size_t sent = 0; sent < line.size();) {
        const ssize_t n = ::send(fd_, line.data() + sent, line.size() - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return closed();
        sent += size_t(n);
    }
}

void LauncherLink::report_healthy(uint32_t data_schema) {
    if (fd_ < 0) return;
    NSLog(@"[Parties] Reporting healthy to the launcher (data schema %u)", data_schema);
    send(update::link::Healthy{data_schema});
}

bool LauncherLink::poll_exit_requested() {
    bool requested = false;
    while (fd_ >= 0) {
        while (auto line = reader_.next()) {
            const auto message = update::link::parse_launcher_message(*line);
            if (message && std::holds_alternative<update::link::ExitRequested>(*message)) {
                NSLog(@"[Parties] The launcher asked the app to exit");
                requested = true;
            }
        }
        char buffer[4096];
        const ssize_t n = recv(fd_, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) {
            closed();
            break;
        }
        reader_.append({buffer, size_t(n)});
    }
    return requested;
}

void HealthReporter::lobby_loaded(MTKView* view) {
    lobby_loaded_ = true;
    if (kUpdateTesting)
        std::fprintf(stderr, "[update-testing] lobby loaded gpu=\"%s\" at %s\n", view.device.name.UTF8String,
                     time_of_day().c_str());
    report_if_healthy();
    if (kUpdateTesting) report_waiting(view);
}

void HealthReporter::watch_frame(id<MTLCommandBuffer> buffer, MTKView* view) {
    if (reported_) return;
    [view.currentDrawable addPresentedHandler:^(id<MTLDrawable> drawable) {
        const CFTimeInterval presented_time = drawable.presentedTime;
        dispatch_async(dispatch_get_main_queue(), ^{
            drawable_finished(presented_time, (view.window.occlusionState & NSWindowOcclusionStateVisible) != 0);
        });
    }];
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        const MTLCommandBufferStatus status = completed.status;
        dispatch_async(dispatch_get_main_queue(), ^{ command_buffer_finished(status); });
    }];
}

void HealthReporter::rendering_suspended() {
    rendered("rendering suspended");
}

// presentedTime stays 0 for a drawable that never reached the screen, and also
// for every drawable on a paravirtual GPU, whose screen shows them anyway.
void HealthReporter::drawable_finished(double presented_time, bool window_visible) {
    if (presented_time > 0) return rendered("frame presented");
    if (unpresented_++ == 0 && kUpdateTesting)
        std::fprintf(stderr, "[update-testing] first drawable dropped without presenting at %s\n",
                     time_of_day().c_str());
    if (unpresented_ >= 60 && gpu_errors_ == 0 && window_visible)
        rendered("frames completed in a visible window, presentedTime unavailable");
}

void HealthReporter::command_buffer_finished(MTLCommandBufferStatus status) {
    if (status == MTLCommandBufferStatusCompleted) return;
    if (gpu_errors_++ == 0 && kUpdateTesting)
        std::fprintf(stderr, "[update-testing] first command buffer failed: status=%d\n", int(status));
}

void HealthReporter::rendered(const char* how) {
    if (rendered_) return;
    rendered_ = true;
    if (kUpdateTesting) std::fprintf(stderr, "[update-testing] %s at %s\n", how, time_of_day().c_str());
    report_if_healthy();
}

void HealthReporter::report_if_healthy() {
    if (reported_ || !lobby_loaded_ || !rendered_) return;
    reported_ = true;
    link_.report_healthy(uint32_t(schema::kCurrent.version));
    if (!kUpdateTesting) return;
    std::fprintf(stderr, "[update-testing] healthy pid=%d at %s\n", getpid(), time_of_day().c_str());
    // An unmanaged run has no launcher to send exit_requested.
    if (const char* delay = std::getenv("PARTIES_TEST_EXIT_AFTER_HEALTHY_MS")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, std::atoll(delay) * int64_t(NSEC_PER_MSEC)),
                       dispatch_get_main_queue(), ^{
            std::fprintf(stderr, "[update-testing] quitting\n");
            [NSApp terminate:nil];
        });
    }
}

// Until healthy, says every 5 s whether the view draws and the window server
// composites the window.
void HealthReporter::report_waiting(MTKView* view) {
    if (reported_) return;
    NSWindow* window = view.window;
    std::fprintf(stderr,
                 "[update-testing] waiting: unpresented=%llu gpu_errors=%llu paused=%d window_visible=%d "
                 "occlusion_visible=%d app_active=%d display_asleep=%d drawable=%.0fx%.0f\n",
                 (unsigned long long)unpresented_, (unsigned long long)gpu_errors_, view.paused, window.visible,
                 (window.occlusionState & NSWindowOcclusionStateVisible) != 0, NSApp.active,
                 CGDisplayIsAsleep(CGMainDisplayID()), view.drawableSize.width, view.drawableSize.height);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * int64_t(NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        report_waiting(view);
    });
}

MacRuntime::MacRuntime(const PartiesHostApi& host) : host_(host), link_(host.link_fd), health_(link_) {}

void MacRuntime::refuse_to_start(ExitCode code, const std::filesystem::path& where, const std::string& reason) const {
    NSString* location = @(where.c_str());
    NSString* why = @(reason.c_str());
    NSLog(@"[Parties] Refusing to start (exit code %d): %@ (%@)", int(code), why, location);
    // Nobody answers a dialog in a testing build, as in the Windows
    // launcher's testing builds.
    if (!kUpdateTesting) {
        NSAlert* alert = [[[NSAlert alloc] init] autorelease];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = code == ExitCode::DataTooNew
            ? @"Your Parties data was saved by a newer version of Parties. Install the latest version to open it."
            : @"Parties cannot start because it cannot open its data.";
        alert.informativeText = [NSString stringWithFormat:@"Location: %@\n\nReason: %@", location, why];
        [NSApp activateIgnoringOtherApps:YES];
        [alert runModal];
    }
    host_.exit_process(int(code));
    std::abort();
}

void MacRuntime::init_failed(const char* what) const {
    NSLog(@"[Parties] Cannot start (exit code %d): %s", int(ExitCode::InitFailed), what);
    host_.exit_process(int(ExitCode::InitFailed));
    std::abort();
}

} // namespace parties::client::macos
