#pragma once

#include <filesystem>

namespace parties::update::launcher {

namespace fs = std::filesystem;

// %LOCALAPPDATA%\Programs\Parties, or --install-root.
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

// %LOCALAPPDATA%\Parties, or --data-root.
struct DataPaths {
    fs::path root;

    fs::path data() const { return root / "data"; }
    fs::path db() const { return data() / "parties_client.db"; }
    fs::path backups() const { return root / "backups"; }
    fs::path trust() const { return root / "update" / "trust.json"; }
    // Public keys (hex, one per line) of portable identities the user chose against.
    fs::path declined_identities() const { return root / "update" / "declined-identities.txt"; }
    fs::path logs() const { return root / "logs"; }
};

// Empty when Windows cannot name the per-user folders.
fs::path default_install_root();
fs::path default_data_root();

} // namespace parties::update::launcher
