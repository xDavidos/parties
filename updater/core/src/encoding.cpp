#include "encoding.h"

#include <cstdio>

namespace parties::update {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::optional<int> digits(std::string_view text, size_t pos, size_t count) {
    int value = 0;
    for (size_t i = pos; i < pos + count; ++i) {
        if (text[i] < '0' || text[i] > '9') return std::nullopt;
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

} // namespace

std::string base64_encode(std::span<const uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t n = uint32_t(bytes[i]) << 16;
        if (i + 1 < bytes.size()) n |= uint32_t(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) n |= bytes[i + 2];
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[n & 63] : '=');
    }
    return out;
}

std::optional<std::vector<uint8_t>> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        bool last = i + 4 == text.size();
        int pad = 0;
        if (last && text[i + 3] == '=') pad = text[i + 2] == '=' ? 2 : 1;
        uint32_t n = 0;
        for (size_t j = 0; j < 4; ++j) {
            int v = j >= 4 - size_t(pad) ? 0 : decode_char(text[i + j]);
            if (v < 0) return std::nullopt;
            n = (n << 6) | uint32_t(v);
        }
        if (pad == 2 && (n & 0xffff) != 0) return std::nullopt;
        if (pad == 1 && (n & 0xff) != 0) return std::nullopt;
        out.push_back(uint8_t(n >> 16));
        if (pad < 2) out.push_back(uint8_t(n >> 8));
        if (pad < 1) out.push_back(uint8_t(n));
    }
    return out;
}

std::string format_utc(std::chrono::sys_seconds t) {
    using namespace std::chrono;
    auto day = floor<days>(t);
    year_month_day ymd{day};
    hh_mm_ss hms{t - day};
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", int(ymd.year()),
                  unsigned(ymd.month()), unsigned(ymd.day()), int(hms.hours().count()),
                  int(hms.minutes().count()), int(hms.seconds().count()));
    return buf;
}

std::optional<std::chrono::sys_seconds> parse_utc(std::string_view text) {
    using namespace std::chrono;
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z')
        return std::nullopt;
    auto y = digits(text, 0, 4), mo = digits(text, 5, 2), d = digits(text, 8, 2);
    auto h = digits(text, 11, 2), mi = digits(text, 14, 2), s = digits(text, 17, 2);
    if (!y || !mo || !d || !h || !mi || !s) return std::nullopt;
    year_month_day ymd{year{*y}, month{unsigned(*mo)}, day{unsigned(*d)}};
    if (!ymd.ok() || *h > 23 || *mi > 59 || *s > 59) return std::nullopt;
    return sys_days{ymd} + hours{*h} + minutes{*mi} + seconds{*s};
}

} // namespace parties::update
