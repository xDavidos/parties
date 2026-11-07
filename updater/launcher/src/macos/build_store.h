#pragma once

// Sole owner of builds/ and downloads/ under the runtime root. A directory
// named builds/<version>-<sha8> is complete by construction: every file lands
// in builds/.staging-<name>, is checked there against the signed entry, and
// the directory is renamed only when whole. Two sources feed the one seal: a
// download from the update feed, and the seed build in the host bundle.
//
// Signed code is never repaired in place. A sealed directory whose files no
// longer match is set aside as .trash-* and sealed again from scratch.

#include "install_paths.h"
#include "update_source.h"

#include <parties/update/supervisor_state.h>

#include <atomic>
#include <expected>
#include <filesystem>
#include <string>
#include <variant>

namespace parties::update::launcher::macos {

namespace fs = std::filesystem;

inline constexpr std::string_view kBuildEntryFile = "build.json";

struct FeedFiles {
    UpdateSource& source;
    const Progress& progress;
    const std::atomic<bool>& cancel;
};
// Parties.app/Contents/Frameworks, holding each of the seed entry's files.
struct SeedFiles {
    fs::path frameworks;
};
using FileSource = std::variant<FeedFiles, SeedFiles>;

class BuildStore {
public:
    explicit BuildStore(InstallPaths install) : install_(std::move(install)) {}

    fs::path dir_of(const BuildId& id) const { return install_.builds() / id.dir_name(); }

    // A sealed directory's record, from its build.json, once every file matches.
    std::expected<BuildRecord, std::string> adopt(const fs::path& dir) const;

    // Idempotent: a build already sealed and intact returns the same record.
    std::expected<BuildRecord, std::string> seal(const BuildEntry& entry, const FileSource& files) const;

    // Refuses every build the committed state still needs.
    bool remove(const BuildId& id, const SupervisorState& committed, std::string& error) const;

    // Deletes leftovers (downloads/, .staging-*, .trash-*) and sealed
    // directories the committed state does not track. Never touches
    // `keep_dir`, the running launcher's own directory. Run only while
    // nothing is being sealed.
    void collect_garbage(const SupervisorState& committed, const fs::path& keep_dir) const;

private:
    std::expected<void, std::string> stage(const FileEntry& file, const FileSource& files,
                                           const fs::path& dest) const;

    InstallPaths install_;
};

} // namespace parties::update::launcher::macos
