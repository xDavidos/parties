// A stand-in for PartiesClient.dylib in update_front_door_e2e. The file
// "scenario" in the directory it was loaded from names what it does once
// AppKit has finished launching.

#import <AppKit/AppKit.h>

#include <parties/host_api.h>
#include <parties/update/link.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace update = parties::update;

namespace {

const PartiesHostApi* g_host = nullptr;

void send_link(const update::link::AppMessage& message) {
    const std::string line = update::link::write(message);
    if (g_host->link_fd >= 0) send(g_host->link_fd, line.data(), line.size(), 0);
}

[[noreturn]] void hang() {
    for (;;) pause();
}

void run(const std::string& scenario) {
    const std::map<std::string, int> exit_codes{{"exit2", 2}, {"exit3", 3}, {"exit4", 4}, {"exit7", 7}};
    if (auto it = exit_codes.find(scenario); it != exit_codes.end()) g_host->exit_process(it->second);

    if (scenario == "clean") [NSApp terminate:nil];
    if (scenario == "healthy_then_quit") {
        send_link(update::link::Hello{update::kLinkProtocol, update::kMinLauncherLink, *update::Version::parse("0.0.0.1")});
        send_link(update::link::Healthy{1});
        [NSApp terminate:nil];
    }
    if (scenario == "return") {
        [NSApp stop:nil];
        [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                                       modifierFlags:0 timestamp:0 windowNumber:0 context:nil
                                             subtype:0 data1:0 data2:0]
                 atStart:YES];
    }
    if (scenario == "crash") *static_cast<volatile int*>(nullptr) = 1;
    if (scenario == "abort") std::abort();
    if (scenario == "stray_exit") std::exit(0);
    if (scenario == "hang") hang();
    if (scenario == "teardown_hang") {
        std::atexit([] { hang(); });
        [NSApp terminate:nil];
    }
    // "wait": stay up until killed or told to quit.
}

} // namespace

@interface FakeClientDelegate : NSObject <NSApplicationDelegate>
@end

@implementation FakeClientDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    std::string scenario;
    std::ifstream(std::string(g_host->build_dir) + "/scenario") >> scenario;
    std::fprintf(stderr, "[fake-client] pid=%d scenario=%s link_fd=%d data_dir=%s\n", getpid(), scenario.c_str(),
                 g_host->link_fd, g_host->data_dir);
    run(scenario);
}
@end

extern "C" const uint32_t parties_client_min_host_abi = 1;

extern "C" int parties_client_main(const PartiesHostApi* host) {
    g_host = host;
    NSApplication* app = [NSApplication sharedApplication];
    app.delegate = [[FakeClientDelegate alloc] init];
    [app run];
    std::fprintf(stderr, "[fake-client] run loop returned\n");
    return 0;
}
