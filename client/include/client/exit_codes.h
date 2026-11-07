#pragma once

namespace parties::client {

// Process exit codes a supervising launcher interprets, so the values are a
// contract with it and must not be renumbered.
enum class ExitCode : int {
    InitFailed      = 2,
    DataUnavailable = 3,
    DataTooNew      = 4,
};

} // namespace parties::client
