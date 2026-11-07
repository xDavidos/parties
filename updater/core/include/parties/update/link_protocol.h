#pragma once

#include <cstdint>

namespace parties::update {

inline constexpr uint32_t kLinkProtocol = 1;
// The oldest launcher link protocol the app still negotiates down to.
inline constexpr uint32_t kMinLauncherLink = 1;

} // namespace parties::update
