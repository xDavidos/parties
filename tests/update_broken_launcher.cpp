// A PartiesLauncher.dylib that never comes up, for update_macos_e2e's boot
// scenarios. Built twice: one copy ends as soon as the host calls it, the
// other (BROKEN_LAUNCHER_HANGS) never returns and never listens.

#include <parties/host_api.h>

#include <unistd.h>

extern "C" const uint32_t parties_launcher_min_host_abi = 1;

extern "C" int parties_launcher_main(const PartiesHostApi*) {
#ifdef BROKEN_LAUNCHER_HANGS
    for (;;) pause();
#else
    return 1;
#endif
}
