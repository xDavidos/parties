#pragma once

#include <cstdint>

namespace parties::update {

// The launcher <-> app link protocol this source tree speaks. The app's hello,
// the launcher's welcome and the build's manifest entry all read these, so a
// protocol bump changes exactly one line.
inline constexpr uint32_t kLinkProtocol = 1;
// The oldest launcher link protocol the app still negotiates down to.
inline constexpr uint32_t kMinLauncherLink = 1;

} // namespace parties::update
