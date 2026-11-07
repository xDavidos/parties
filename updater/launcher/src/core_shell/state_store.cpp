#include "state_store.h"

#include "durable_file.h"
#include "log.h"

#include <parties/update/state_codec.h>

namespace parties::update::launcher {

SupervisorState adopted_state(const BuildRecord& installed) {
    SupervisorState s = initial_state(installed, Channel::Stable);
    s.generation = 1;
    return s;
}

StateStore::StateStore(InstallPaths install, std::string data_dir)
    : install_(std::move(install)), data_dir_(std::move(data_dir)) {}

LoadResult StateStore::load() {
    std::optional<std::string> main = read_file(install_.supervisor());
    std::optional<std::string> prev = read_file(install_.supervisor_prev());
    if (!main && !prev) return NoState{};
    if (main) {
        auto parsed = parse_state(*main);
        if (parsed) return *parsed;
        if (parsed.error() == StateParseError::FutureSchema) return NewerLauncherState{};
        log("supervisor.json is unreadable (error {}), trying .prev", static_cast<int>(parsed.error()));
    }
    if (prev) {
        auto parsed = parse_state(*prev);
        if (parsed) {
            main_is_good_ = false;
            log("recovered state from supervisor.json.prev (generation {})", parsed->generation);
            return *parsed;
        }
        if (parsed.error() == StateParseError::FutureSchema) return NewerLauncherState{};
    }
    return UnreadableState{};
}

bool StateStore::commit(const SupervisorState& state, std::string& error) {
    std::error_code ec;
    fs::create_directories(install_.state(), ec);
    if (main_is_good_) {
        if (auto current = read_file(install_.supervisor())) {
            if (!durable_replace(install_.supervisor_prev(), *current, error)) return false;
        }
    }
    if (!durable_replace(install_.supervisor(), write_state(state), error)) return false;
    main_is_good_ = true;
    return durable_replace(install_.boot_contract(),
                           boot::write_boot_contract(derive_boot_contract(state, data_dir_)), error);
}

bool StateStore::write_session(const boot::SessionReport& report, std::string& error) {
    std::error_code ec;
    fs::create_directories(install_.state(), ec);
    return durable_replace(install_.session(), boot::write_session_report(report), error);
}

} // namespace parties::update::launcher
