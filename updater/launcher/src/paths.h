#pragma once

#include "install_paths.h"

#include <filesystem>

namespace parties::update::launcher {

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
