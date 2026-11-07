#include "install_plan.h"

#include <algorithm>

namespace parties::update::launcher {

InstallPlan plan_install(const LoadResult& loaded, const BuildRecord& bundled) {
    const auto* state = std::get_if<SupervisorState>(&loaded);
    if (!state) {
        if (std::holds_alternative<NewerLauncherState>(loaded)) return {Keep{}, false, false};
        return {Adopt{}, true, true};
    }
    if (state->find(bundled.id)) return {Keep{}, true, false};
    const auto* staged = std::get_if<Staged>(&state->activation);
    const Version& newest = staged && staged->build.version > state->good.version ? staged->build.version
                                                                                 : state->good.version;
    if (bundled.id.version > newest) {
        Step sealed = step(*state, BuildSealed{bundled, std::nullopt}, Context{});
        bool commits = std::ranges::any_of(sealed.effects, [](const Effect& e) {
            return std::holds_alternative<Commit>(e);
        });
        // A candidate under probation is never replaced from outside.
        if (commits) return {Stage{std::move(sealed)}, true, true};
    }
    return {Keep{}, false, false};
}

} // namespace parties::update::launcher
