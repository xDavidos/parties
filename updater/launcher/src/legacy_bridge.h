#pragma once

// The move from the portable zip. The legacy in-app updater replaces the
// zip's parties_client.exe with this setup (the carrier) and starts it with
// --update-cleanup <old exe>; users and old shortcuts start it later as
// plain parties_client.exe. Each start installs or repairs, then moves the
// portable copy's data into the data root copy-first.

#include "paths.h"

#include <client/legacy_import.h>

#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace parties::update::launcher {

// The binary the legacy updater left behind.
struct OldBinary {
    enum class Kind {
        Cleanup,  // --update-cleanup: parties_client.old.exe, deleted once the move is done
        Replace,  // --update-replace: the old parties_client.exe, overwritten with the carrier
    };
    fs::path path;
    Kind kind = Kind::Cleanup;
};

struct LegacyHints {
    fs::path working_dir;  // as started, before anything changes it
    fs::path self_dir;
    std::optional<OldBinary> old;
};

// Every folder a portable copy may keep parties_client.db in, in search
// order: the working directory, the old binary's folder, this exe's folder,
// the folders of links to a portable copy (portable_shortcut_dirs), and the
// folders of running portable copies. Read before setup rewrites the Start
// menu shortcut. Needs COM on the calling thread.
std::vector<fs::path> legacy_dirs(const LegacyHints& hints);

// Waits until no portable copy holds Local\<prefix>.Client.Instance. False
// when one still does after `timeout`.
bool wait_for_portable_exit(const std::wstring& object_prefix, std::chrono::milliseconds timeout);

struct LegacyData {
    std::string error;  // empty: the data root holds every legacy database it should
    std::string note;   // for the user, or empty
};

enum class IdentityChoice { KeepLive, UseLegacy };
// Asks the user which identity to keep. Empty: nobody can be asked (--silent).
using ChooseIdentity = std::function<IdentityChoice(const client::IdentityConflict&)>;

// Imports the best legacy database when the data root has none, and keeps a
// copy of every legacy database found under backups\legacy-*. Sources are
// opened read-only and never changed. When the live database has another
// identity (or none) than a portable one it never held, `choose` decides
// which stays live; the other stays a backup. Unasked, the live database
// stays and the note says so.
// Repeating it writes nothing new.
LegacyData move_legacy_data(const DataPaths& data, std::span<const fs::path> dirs, const ChooseIdentity& choose);

// Deletes parties_client.old.exe, or puts the carrier in place of the old
// parties_client.exe. Only names the legacy updater uses are touched.
void retire_old_binary(const OldBinary& old, const fs::path& self);

} // namespace parties::update::launcher
