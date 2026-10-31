#pragma once

#include <client/data_location.h>
#include <parties/types.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace parties::client {

// A parties_client.db found where the portable zip build used to keep it.
struct LegacyCandidate {
    std::filesystem::path path;
    bool readable = false;                    // opened read-only and passed integrity_check
    std::optional<PublicKey> identity;
    std::filesystem::file_time_type modified{};  // newest of the file and its -wal
};

struct LegacyImportPlan {
    std::optional<std::size_t> import;   // candidate that becomes the live database
    std::vector<std::size_t> preserve;   // every other candidate, copied to backups
};

// Imports the readable candidate with an identity and the newest modification
// time; nothing found is ever dropped.
LegacyImportPlan plan_legacy_import(std::span<const LegacyCandidate> candidates);

struct LegacyImportResult {
    enum class Status {
        Skipped,        // the data directory already has a database
        NoneImported,   // nothing readable was found (unreadable files are still preserved)
        Imported,
        Failed,         // nothing was imported; the next start retries
    };
    Status status = Status::Skipped;
    std::filesystem::path imported_from;
    std::vector<std::filesystem::path> preserved;  // backups this run wrote
    std::string error;
};

// One-time, copy-first move of a legacy database into `location`. Sources are
// opened read-only and never modified. Runs only while location.db_path() does
// not exist, so repeating it is a no-op; a crash leaves no database (retried
// next start) or a complete one.
LegacyImportResult import_legacy_data(const DataLocation& location,
                                      std::span<const std::filesystem::path> search_dirs);

// Copies every legacy database found to backups\legacy-* and imports
// nothing, whether or not the data directory has a database. A source whose
// copy is already there is not copied again, so a repeat writes nothing.
// Status is Skipped, or Failed when a copy could not be written.
LegacyImportResult preserve_legacy_data(const DataLocation& location,
                                        std::span<const std::filesystem::path> search_dirs);

} // namespace parties::client
