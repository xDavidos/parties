#pragma once

#include <cstdint>

namespace parties::update {

// What a database says about itself (the single-row parties_schema table).
// compat_floor is the oldest build schema that can still read and write it.
// A database without the table is legacy(): {1, 1}.
struct DataInfo {
    uint32_t version = 1;
    uint32_t compat_floor = 1;

    static constexpr DataInfo legacy() { return {1, 1}; }
    friend constexpr bool operator==(const DataInfo&, const DataInfo&) = default;
};

enum class Compatibility : uint8_t {
    MigratesForward,
    ReadsAsIs,        // same schema, or newer data whose floor admits the build
    Incompatible,
};

// A build never lowers a newer database's version or floor.
constexpr Compatibility compatibility(uint32_t build_schema, DataInfo db) {
    if (build_schema > db.version) return Compatibility::MigratesForward;
    if (build_schema == db.version || db.compat_floor <= build_schema)
        return Compatibility::ReadsAsIs;
    return Compatibility::Incompatible;
}

} // namespace parties::update
