#include <parties/update/manifest.h>

namespace parties::update {

std::optional<Channel> parse_channel(std::string_view name) {
    if (name == "stable") return Channel::Stable;
    if (name == "beta") return Channel::Beta;
    return std::nullopt;
}

std::string_view channel_name(Channel channel) {
    return channel == Channel::Beta ? "beta" : "stable";
}

} // namespace parties::update
