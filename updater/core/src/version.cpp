#include <parties/update/version.h>

#include <limits>

namespace parties::update {

namespace {

std::optional<uint32_t> parse_part(std::string_view text) {
    if (text.empty() || text.size() > 10) return std::nullopt;
    if (text.size() > 1 && text[0] == '0') return std::nullopt;
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (value > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    return static_cast<uint32_t>(value);
}

} // namespace

std::optional<Version> Version::parse(std::string_view text) {
    Version v;
    for (size_t i = 0; i < 4; ++i) {
        size_t dot = text.find('.');
        bool last = i == 3;
        if (last != (dot == std::string_view::npos)) return std::nullopt;
        auto part = parse_part(text.substr(0, dot));
        if (!part) return std::nullopt;
        v.parts[i] = *part;
        if (!last) text.remove_prefix(dot + 1);
    }
    return v;
}

std::string Version::to_string() const {
    return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." +
           std::to_string(parts[2]) + "." + std::to_string(parts[3]);
}

std::string BuildId::dir_name() const {
    return version.to_string() + "-" + digest.short_hex();
}

} // namespace parties::update
