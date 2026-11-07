#pragma once

// Each part converges: running the same setup twice finds nothing left to do the
// second time.

#include "state_store.h"

#include <parties/update/probation.h>

#include <variant>

namespace parties::update::launcher {

struct Adopt {
    friend bool operator==(const Adopt&, const Adopt&) = default;
};
// The bundled build is unknown and newer than both good and any staged
// build: it is staged the way a download would be, so it runs under
// probation and good stays good.
struct Stage {
    Step step;
};
struct Keep {
    friend bool operator==(const Keep&, const Keep&) = default;
};
using StatePlan = std::variant<Adopt, Stage, Keep>;

struct InstallPlan {
    StatePlan state;
    // False for a bundled build the state would not track, which the launcher would only collect.
    bool seal_bundled = false;
    // Overwrite a differing Parties.exe. Otherwise it is only restored when
    // missing, so an older setup never replaces a newer stub.
    bool replace_stub = false;
};

InstallPlan plan_install(const LoadResult& loaded, const BuildRecord& bundled);

} // namespace parties::update::launcher
