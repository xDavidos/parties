#pragma once

#include <parties/host_api.h>

namespace parties::client::macos {

// What the host started this payload with. Valid for the whole process.
const PartiesHostApi& host();

} // namespace parties::client::macos
