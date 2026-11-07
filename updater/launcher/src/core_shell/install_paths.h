#pragma once

#include <filesystem>

namespace parties::update::launcher {

namespace fs = std::filesystem;

struct InstallPaths {
    fs::path root;

    fs::path builds() const { return root / "builds"; }
    fs::path downloads() const { return root / "downloads"; }
    fs::path state() const { return root / "state"; }
    fs::path supervisor() const { return state() / "supervisor.json"; }
    fs::path supervisor_prev() const { return state() / "supervisor.json.prev"; }
    fs::path boot_contract() const { return state() / "boot-contract.json"; }
    fs::path session() const { return state() / "launcher-session.json"; }
};

} // namespace parties::update::launcher
