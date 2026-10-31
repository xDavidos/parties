#include <client/test_pattern.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace parties::client {

namespace {

struct Bgr { uint8_t b, g, r; };

constexpr std::array<Bgr, 8> kBars = {{
    {191, 191, 191}, {0, 191, 191}, {191, 191, 0}, {0, 191, 0},
    {191, 0, 191}, {0, 0, 191}, {191, 0, 0}, {0, 0, 0},
}};

// Segment bits a..g for 0-9.
constexpr std::array<uint8_t, 10> kDigitSegments = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

uint8_t crc8(uint32_t value) {
    // A non-zero seed keeps an all-black or all-white rectangle from decoding.
    uint8_t crc = 0x5A;
    for (int i = 0; i < 3; ++i) {
        crc ^= static_cast<uint8_t>(value >> (8 * i));
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

uint32_t encode_word(uint32_t counter) {
    counter &= kTestPatternCounterMask;
    return counter | (static_cast<uint32_t>(crc8(counter)) << kTestPatternCounterBits);
}

void fill(std::vector<uint8_t>& image, uint32_t width, uint32_t height,
          int x0, int y0, int x1, int y1, Bgr colour) {
    x0 = std::clamp(x0, 0, static_cast<int>(width));
    x1 = std::clamp(x1, 0, static_cast<int>(width));
    y0 = std::clamp(y0, 0, static_cast<int>(height));
    y1 = std::clamp(y1, 0, static_cast<int>(height));
    for (int y = y0; y < y1; ++y) {
        uint8_t* row = image.data() + (static_cast<size_t>(y) * width + x0) * 4;
        for (int x = x0; x < x1; ++x, row += 4) {
            row[0] = colour.b; row[1] = colour.g; row[2] = colour.r; row[3] = 255;
        }
    }
}

struct CellRect { uint32_t x0, y0, x1, y1; };

CellRect cell_rect(uint32_t width, uint32_t height, uint32_t index) {
    const uint32_t column = index % kTestPatternColumns;
    const uint32_t row = index / kTestPatternColumns;
    const uint32_t band_top = height * 3 / 4;
    const uint32_t band_height = height - band_top;
    return {
        column * width / kTestPatternColumns,
        band_top + row * band_height / kTestPatternRows,
        (column + 1) * width / kTestPatternColumns,
        band_top + (row + 1) * band_height / kTestPatternRows,
    };
}

}

TestPatternRenderer::TestPatternRenderer(uint32_t width, uint32_t height, bool noise)
    : width_(width), height_(height), noise_(noise),
      background_(static_cast<size_t>(width) * height * 4),
      frame_(background_.size()) {
    const int bars_bottom = static_cast<int>(height * 3 / 4);
    for (uint32_t i = 0; i < kBars.size(); ++i) {
        fill(background_, width, height,
             static_cast<int>(i * width / kBars.size()), 0,
             static_cast<int>((i + 1) * width / kBars.size()), bars_bottom, kBars[i]);
    }
    fill(background_, width, height, 0, bars_bottom, static_cast<int>(width),
         static_cast<int>(height), {128, 128, 128});
}

const uint8_t* TestPatternRenderer::render(uint32_t counter) {
    render_changes(counter);
    return frame_.data();
}

const std::vector<TestPatternRegion>& TestPatternRenderer::render_changes(uint32_t counter) {
    const int w = static_cast<int>(width_);
    const int h = static_cast<int>(height_);
    const int bars_bottom = h * 3 / 4;
    changes_.clear();
    if (previous_stripe_) {
        const TestPatternRegion& old = *previous_stripe_;
        for (uint32_t y = old.y0; y < old.y1; ++y) {
            const size_t offset = (static_cast<size_t>(y) * width_ + old.x0) * 4;
            std::memcpy(frame_.data() + offset, background_.data() + offset, (old.x1 - old.x0) * 4);
        }
        changes_.push_back(old);
    } else {
        std::memcpy(frame_.data(), background_.data(), frame_.size());
        changes_.push_back({0, 0, width_, height_});
    }

    if (noise_) {
        const uint32_t x0 = (width_ / 2) & ~1u;
        const uint32_t y1 = height_ / 2;
        uint64_t state = (static_cast<uint64_t>(counter) + 1) * 0x9E3779B97F4A7C15ull;
        for (uint32_t y = 0; y < y1; ++y) {
            uint8_t* row = frame_.data() + (static_cast<size_t>(y) * width_ + x0) * 4;
            for (uint32_t x = x0; x + 1 < width_; x += 2, row += 8) {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                const uint64_t pixels = state | 0xFF000000FF000000ull;
                std::memcpy(row, &pixels, 8);
            }
        }
        changes_.push_back({x0, 0, width_ & ~1u, y1});
    }

    const int stripe_width = (std::max)(w / 80, 4);
    const int stripe_x = static_cast<int>((static_cast<uint64_t>(counter) * (w / 120 + 1)) % w);
    fill(frame_, width_, height_, stripe_x, 0, stripe_x + stripe_width, bars_bottom, {255, 255, 255});
    previous_stripe_ = TestPatternRegion{static_cast<uint32_t>(stripe_x), 0,
                                         static_cast<uint32_t>((std::min)(stripe_x + stripe_width, w)),
                                         static_cast<uint32_t>(bars_bottom)};
    changes_.push_back(*previous_stripe_);

    const int digit_h = (std::max)(h / 8, 14);
    const int digit_w = digit_h / 2;
    const int thick = (std::max)(digit_h / 8, 2);
    const int margin = digit_h / 4;
    constexpr int kDigits = 8;
    const int panel_x = margin;
    const int panel_y = margin;
    const int panel_right = (std::min)(panel_x + kDigits * (digit_w + thick * 2) + thick * 2, w);
    const int panel_bottom = (std::min)(panel_y + digit_h + thick * 4, h);
    fill(frame_, width_, height_, panel_x, panel_y, panel_right, panel_bottom, {0, 0, 0});
    changes_.push_back({static_cast<uint32_t>(panel_x), static_cast<uint32_t>(panel_y),
                        static_cast<uint32_t>(panel_right), static_cast<uint32_t>(panel_bottom)});
    uint32_t value = counter % 100'000'000u;
    for (int d = kDigits - 1; d >= 0; --d) {
        const uint8_t segments = kDigitSegments[value % 10];
        value /= 10;
        const int x = panel_x + thick * 2 + d * (digit_w + thick * 2);
        const int y = panel_y + thick * 2;
        const int mid = y + digit_h / 2;
        const Bgr on{255, 255, 255};
        if (segments & 0x01) fill(frame_, width_, height_, x, y, x + digit_w, y + thick, on);
        if (segments & 0x02) fill(frame_, width_, height_, x + digit_w - thick, y, x + digit_w, mid, on);
        if (segments & 0x04) fill(frame_, width_, height_, x + digit_w - thick, mid, x + digit_w, y + digit_h, on);
        if (segments & 0x08) fill(frame_, width_, height_, x, y + digit_h - thick, x + digit_w, y + digit_h, on);
        if (segments & 0x10) fill(frame_, width_, height_, x, mid, x + thick, y + digit_h, on);
        if (segments & 0x20) fill(frame_, width_, height_, x, y, x + thick, mid, on);
        if (segments & 0x40) fill(frame_, width_, height_, x, mid - thick / 2, x + digit_w, mid + thick - thick / 2, on);
    }

    const uint32_t word = encode_word(counter);
    for (uint32_t i = 0; i < kTestPatternColumns * kTestPatternRows; ++i) {
        const CellRect cell = cell_rect(width_, height_, i);
        const uint8_t level = (word >> i) & 1u ? 255 : 0;
        fill(frame_, width_, height_, static_cast<int>(cell.x0), static_cast<int>(cell.y0),
             static_cast<int>(cell.x1), static_cast<int>(cell.y1), {level, level, level});
    }
    changes_.push_back({0, height_ * 3 / 4, width_, height_});
    return changes_;
}

std::optional<uint32_t> decode_test_pattern(const uint8_t* bgra, size_t stride,
                                            uint32_t image_width, uint32_t image_height,
                                            double x, double y, double w, double h) {
    if (!bgra || w < kTestPatternColumns * 2 || h < 16) return std::nullopt;
    constexpr uint32_t kReference = 1000;
    constexpr int kSamples = 5;
    uint32_t word = 0;
    for (uint32_t i = 0; i < kTestPatternColumns * kTestPatternRows; ++i) {
        const CellRect cell = cell_rect(kReference, kReference, i);
        const double cw = cell.x1 - cell.x0;
        const double ch = cell.y1 - cell.y0;
        uint32_t sum = 0;
        uint32_t count = 0;
        for (int sy = 0; sy < kSamples; ++sy) {
            for (int sx = 0; sx < kSamples; ++sx) {
                const double px = cell.x0 + cw * (0.25 + 0.5 * sx / (kSamples - 1));
                const double py = cell.y0 + ch * (0.25 + 0.5 * sy / (kSamples - 1));
                const int ix = static_cast<int>(x + px * w / kReference);
                const int iy = static_cast<int>(y + py * h / kReference);
                if (ix < 0 || iy < 0 || ix >= static_cast<int>(image_width) ||
                    iy >= static_cast<int>(image_height))
                    return std::nullopt;
                const uint8_t* p = bgra + static_cast<size_t>(iy) * stride + static_cast<size_t>(ix) * 4;
                sum += p[0] + p[1] + p[2];
                count += 3;
            }
        }
        if (sum / count >= 128) word |= 1u << i;
    }
    const uint32_t counter = word & kTestPatternCounterMask;
    if ((word >> kTestPatternCounterBits) != crc8(counter)) return std::nullopt;
    return counter;
}

}
