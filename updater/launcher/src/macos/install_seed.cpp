// --install-seed: the macOS twin of setup's install step. The host runs it
// with the bundle's own Frameworks/PartiesLauncher.dylib on a first run, after
// the runtime root was deleted, and when a newer or different Parties.app
// was put in place (process-contract.md, "Front start").

#include "build_store.h"
#include "code_signing.h"
#include "durable_file.h"
#include "install_plan.h"
#include "log.h"
#include "state_store.h"
#include "supervisor.h"
#include "test_hooks.h"

#include <parties/update/product.h>

#include <chrono>

namespace parties::update::launcher::macos {

namespace {

// The plan's state change, executed the way the launcher executes effects.
bool apply(const InstallPlan& plan, const BuildStore& builds, StateStore& store, const BuildId& seed) {
    std::string error;
    if (std::holds_alternative<Adopt>(plan.state)) {
        auto record = builds.adopt(builds.dir_of(seed));
        if (!record) return (log("cannot adopt the seed: {}", record.error()), false);
        if (!store.commit(adopted_state(*record), error)) return (log("cannot save the state: {}", error), false);
        log("adopted {} as the good build", seed.dir_name());
        return true;
    }
    if (const auto* stage = std::get_if<Stage>(&plan.state)) {
        for (const Effect& effect : stage->step.effects) {
            if (const auto* commit = std::get_if<Commit>(&effect)) {
                if (!store.commit(commit->state, error)) return (log("cannot save the state: {}", error), false);
            } else if (const auto* doomed = std::get_if<DeleteBuild>(&effect)) {
                if (!builds.remove(doomed->build, stage->step.next, error)) log("cannot delete: {}", error);
            }
        }
        log("staged {}; {} stays good until it is proven", seed.dir_name(), stage->step.next.good.dir_name());
        return true;
    }
    log("kept the installed builds; {} is not newer", seed.dir_name());
    return true;
}

} // namespace

int run_install_seed(const LauncherOptions& o) {
    const fs::path bundle = *o.install_seed;
    log("installing the seed build of {}", bundle.string());
    // A launcher that a front asked to quit for this install may take a moment to go.
    if (!take_launcher_lock(o.install, std::chrono::seconds(60))) {
        log("another launcher still runs, so the seed cannot be installed now");
        return 1;
    }
    if (auto why = code_requirement_failure(bundle, true)) {
        log("{}", *why);
        return 1;
    }
    const fs::path seed_file = bundle / "Contents" / "Resources" / "seed" / "build.json";
    auto text = read_file(seed_file);
    if (!text) {
        log("{} is missing", seed_file.string());
        return 1;
    }
    auto entry = parse_build_entry(*text, kMacClient);
    if (!entry) {
        log("{} is unreadable: {}", seed_file.string(), entry.error().detail);
        return 1;
    }

    StateStore store(o.install, o.data_root.string());
    BuildStore builds(o.install);
    const BuildRecord seed = BuildRecord::from_entry(*entry, kMacClient);
    const InstallPlan plan = plan_install(store.load(), seed);
    std::error_code ec;
    fs::create_directories(o.install.builds(), ec);

    testing::before_effect();
    if (plan.seal_bundled) {
        auto sealed = builds.seal(*entry, SeedFiles{bundle / "Contents" / "Frameworks"});
        if (!sealed) {
            log("cannot seal the seed: {}", sealed.error());
            return 1;
        }
    }
    testing::before_effect();
    if (!apply(plan, builds, store, seed.id)) return 1;
    testing::before_effect();
    std::string error;
    if (!durable_replace(o.install.state() / "seed.json", "{\"seed\":\"" + seed.id.dir_name() + "\"}\n", error)) {
        log("cannot record the seed: {}", error);
        return 1;
    }
    log("seed {} installed", seed.id.dir_name());
    return 0;
}

} // namespace parties::update::launcher::macos
