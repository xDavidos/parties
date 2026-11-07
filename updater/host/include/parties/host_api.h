// The only interface between the macOS host executable (Parties.app/Contents/
// MacOS/Parties) and the payload dylib it loads. Plain C, so a host and a
// payload from different builds can still talk.
//
// Append-only once a host ships without Sparkle: a field is never removed,
// renamed, retyped or reordered. New fields go at the end, and `size` tells the
// payload which ones exist. PARTIES_HOST_ABI rises only when a payload may
// require a new field, and a payload states the lowest ABI it needs.
//
// The payload calls the host. The host never calls payload code except the
// entry point.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARTIES_HOST_ABI 1u

typedef struct PartiesHostApi {
    uint32_t abi;   // PARTIES_HOST_ABI of the running host
    uint32_t size;  // sizeof(PartiesHostApi) as the host compiled it

    const char* host_bundle;  // absolute path of the running Parties.app
    const char* build_dir;    // absolute directory the payload was loaded from
    const char* data_dir;     // absolute, never null: where parties.db lives
    const char* backups_dir;  // absolute, never null

    // Connected link socket to the launcher, or -1 when unmanaged.
    int link_fd;

    int argc;  // the front's arguments without argv[0] and host flags
    const char* const* argv;

    // Ends the process with `code`. Never returns.
    void (*exit_process)(int code);
} PartiesHostApi;

// Exported by PartiesClient.dylib. The host checks the ABI before it calls
// the entry point.
extern const uint32_t parties_client_min_host_abi;

// Runs the app. Returns only if the run loop ends without terminate:.
int parties_client_main(const PartiesHostApi* host);

// Exported by PartiesLauncher.dylib, run in a process started with
// --role=launcher. argv holds that process's arguments after argv[0], and
// build_dir the directory the dylib was loaded from. Returns the exit code.
extern const uint32_t parties_launcher_min_host_abi;
int parties_launcher_main(const PartiesHostApi* host);

#ifdef __cplusplus
}
#endif
