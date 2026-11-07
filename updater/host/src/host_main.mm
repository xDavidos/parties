// The macOS host: the bundle's main executable. It loads the client payload
// from Contents/Frameworks and runs it in this process, so LaunchServices, the
// Dock and TCC only ever see this executable.

#import <AppKit/AppKit.h>

#include "payload.h"

#include <client/exit_codes.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void exit_process(int code) {
    std::fprintf(stderr, "[host] exit_process(%d)\n", code);
    _exit(code);
}

[[noreturn]] void run_front(int argc, const char* argv[]) {
    const fs::path bundle = NSBundle.mainBundle.bundlePath.fileSystemRepresentation;
    const fs::path build_dir = bundle / "Contents" / "Frameworks";
    const fs::path data_dir = fs::path(NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject.fileSystemRepresentation) / "Parties";
    const fs::path backups_dir = data_dir / "backups";
    std::fprintf(stderr, "[host] front pid=%d bundle=%s\n", getpid(), bundle.c_str());

    const auto payload = parties::host::load(build_dir / "PartiesClient.dylib", std::nullopt);
    if (!payload) exit_process(static_cast<int>(parties::client::ExitCode::InitFailed));

    // terminate: ends the process with exit(0) right after this notification.
    [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationWillTerminateNotification
                                                    object:nil
                                                     queue:nil
                                                usingBlock:^(NSNotification* note) {
        std::fprintf(stderr, "[host] terminate: exiting 0\n");
    }];

    const PartiesHostApi api{
        .abi = PARTIES_HOST_ABI,
        .size = sizeof(PartiesHostApi),
        .host_bundle = bundle.c_str(),
        .build_dir = build_dir.c_str(),
        .data_dir = data_dir.c_str(),
        .backups_dir = backups_dir.c_str(),
        .link_fd = -1,
        .argc = argc - 1,
        .argv = argv + 1,
        .exit_process = &exit_process,
    };
    exit_process(payload->entry(&api));
}

#ifdef PARTIES_UPDATE_TESTING
// Loads a payload without running it: proves the file maps under this
// process's code-signing rules and times the hash and dlopen.
int self_test(const fs::path& file) {
    return parties::host::load(file, std::nullopt) ? 0 : 1;
}
#endif

} // namespace

int main(int argc, const char* argv[]) {
#ifdef PARTIES_UPDATE_TESTING
    constexpr std::string_view self_test_flag = "--self-test=";
    if (argc == 2 && std::string_view(argv[1]).starts_with(self_test_flag))
        return self_test(argv[1] + self_test_flag.size());
#endif
    @autoreleasepool {
        run_front(argc, argv);
    }
}
