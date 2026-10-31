#pragma once

namespace parties::client {

// Process exit codes a supervising launcher interprets, so the values are a
// contract with it and must not be renumbered.
enum class ExitCode : int {
    InitFailed      = 2,   // a platform or window subsystem could not be initialized
    DataUnavailable = 3,   // the data directory or database cannot be opened, created or migrated
    DataTooNew      = 4,   // the database was written by a newer, incompatible build
};

} // namespace parties::client
