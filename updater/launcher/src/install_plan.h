#pragma once

// What install or repair does, decided only from the state on disk and the
// bundled build. Each part converges: running the same setup twice finds
// nothing left to do the second time.

#include "state_store.h"

#include <parties/update/probation.h>

#include <variant>

namespace parties::update::launcher {

// No usable state (fresh install, lost or unreadable state): the bundled
// build becomes good, as on the launcher's first run.
struct Adopt {
    friend bool operator==(const Adopt&, const Adopt&) = default;
};
// The bundled build is newer than good and unknown: it is staged the way a
// download would be, so it runs under probation and good stays good.
struct Stage {
    Step step;  // BuildSealed's transition
};
// Anything else: the state is left as it is.
struct Keep {
    friend bool operator==(const Keep&, const Keep&) = default;
};
using StatePlan = std::variant<Adopt, Stage, Keep>;

struct InstallPlan {
    StatePlan state;
    // Seal builds\<bundled>\, repairing it if damaged. False for a bundled
    // build the state would not track, which the launcher would only collect.
    bool seal_bundled = false;
    // Overwrite a differing Parties.exe. Otherwise it is only restored when
    // missing, so an older setup never replaces a newer stub.
    bool replace_stub = false;
};

InstallPlan plan_install(const LoadResult& loaded, const BuildRecord& bundled);

} // namespace parties::update::launcher
