// PartiesClient.dylib's entry point. The host executable (updater/host) loads
// this payload and calls parties_client_main on its main thread.

#import <AppKit/AppKit.h>
#import "PartiesAppDelegate.h"
#include "mac_runtime.h"

extern "C" const uint32_t parties_client_min_host_abi = 1;

static parties::client::macos::MacRuntime* g_runtime = nullptr;

parties::client::macos::MacRuntime& parties::client::macos::runtime()
{
    return *g_runtime;
}

extern "C" int parties_client_main(const PartiesHostApi* host)
{
    // Never freed: the process ends through the host.
    g_runtime = new parties::client::macos::MacRuntime(*host);
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        PartiesAppDelegate* delegate = [[PartiesAppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
