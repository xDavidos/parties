#include <parties/update/supervisor_state.h>

#include <algorithm>

namespace parties::update {

BuildRecord BuildRecord::from_entry(const BuildEntry& entry, const ProductSpec& product) {
    BuildRecord r;
    r.id = entry.id();
    r.data_schema = entry.data_schema;
    r.data_compat_floor = entry.data_compat_floor;
    r.link_protocol = entry.link_protocol;
    r.min_supported_launcher_link = entry.min_supported_launcher_link;
    if (const FileEntry* f = entry.file(product.launcher_file)) r.launcher_sha256 = f->sha256;
    if (const FileEntry* f = entry.file(product.app_file)) r.app_sha256 = f->sha256;
    return r;
}

bool Revocations::contains(const Version& version) const {
    return std::ranges::find(stable, version) != stable.end() || std::ranges::find(beta, version) != beta.end();
}

const BuildRecord* SupervisorState::find(const BuildId& id) const {
    auto it = std::ranges::find(builds, id, &BuildRecord::id);
    return it == builds.end() ? nullptr : &*it;
}

SupervisorState initial_state(const BuildRecord& installed, Channel channel) {
    SupervisorState s;
    s.channel = channel;
    s.good = installed.id;
    s.builds.push_back(installed);
    s.builds.back().verdict = Proven{};
    return s;
}

std::string build_dir(const BuildId& id) {
    return "builds\\" + id.dir_name();
}

boot::BootContract derive_boot_contract(const SupervisorState& state, std::string_view data_dir) {
    boot::BootContract c;
    const BuildRecord* good = state.find(state.good);
    c.good = {build_dir(state.good), good ? good->launcher_sha256 : Sha256{}};
    c.last_good_app = {build_dir(state.good), good ? good->app_sha256 : Sha256{}};
    if (state.previous_good) {
        if (const BuildRecord* prev = state.find(*state.previous_good))
            c.previous_good = boot::LauncherRef{build_dir(prev->id), prev->launcher_sha256};
    }
    c.data_dir = data_dir;
    return c;
}

} // namespace parties::update
