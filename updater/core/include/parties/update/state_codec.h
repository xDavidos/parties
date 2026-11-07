#pragma once

// JSON for the launcher's persisted state: state\supervisor.json and
// update\trust.json. Unknown fields are ignored. A higher schema is refused
// with FutureSchema so an older launcher (previous_good's, run by the stub
// fallback) never acts on state it does not understand; it must leave the file
// alone. Bump the schema only for changes an older reader would misread.

#include <parties/update/manifest.h>
#include <parties/update/supervisor_state.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace parties::update {

inline constexpr uint32_t kStateSchema = 1;

enum class StateParseError : uint8_t {
    Malformed,     // unreadable, or a field has the wrong shape
    FutureSchema,  // written by a newer launcher
    Inconsistent,  // readable, but references do not resolve (e.g. good has no record)
};

std::string write_state(const SupervisorState& state);
std::expected<SupervisorState, StateParseError> parse_state(std::string_view text);

struct TrustFile {
    TrustState stable;
    TrustState beta;

    TrustState& of(Channel c) { return c == Channel::Beta ? beta : stable; }
    const TrustState& of(Channel c) const { return c == Channel::Beta ? beta : stable; }
    friend bool operator==(const TrustFile&, const TrustFile&) = default;
};

std::string write_trust(const TrustFile& trust);
std::expected<TrustFile, StateParseError> parse_trust(std::string_view text);

} // namespace parties::update
