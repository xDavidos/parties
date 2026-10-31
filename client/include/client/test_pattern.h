#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace parties::client {

// Deterministic synthetic share source. The bottom quarter carries the frame
// counter as a 16x2 grid of black/white cells (24 counter bits + CRC-8), large
// enough to survive encoding and any downscale the viewer applies.
inline constexpr uint32_t kTestPatternColumns = 16;
inline constexpr uint32_t kTestPatternRows = 2;
inline constexpr uint32_t kTestPatternCounterBits = 24;
inline constexpr uint32_t kTestPatternCounterMask = (1u << kTestPatternCounterBits) - 1;

struct TestPatternRegion {
    uint32_t x0, y0, x1, y1;
};

class TestPatternRenderer {
public:
    // With `noise`, the top-right quarter carries fresh random pixels every frame,
    // content no encoder can predict, so bitrate and codec choices show up.
    TestPatternRenderer(uint32_t width, uint32_t height, bool noise = false);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    size_t stride() const { return static_cast<size_t>(width_) * 4; }

    // Renders frame `counter` as BGRA and returns the buffer (valid until the next call).
    const uint8_t* render(uint32_t counter);
    // Same as render(), and lists the rectangles that differ from the previous
    // call so a 4K source only uploads what changed. The first call covers the frame.
    const std::vector<TestPatternRegion>& render_changes(uint32_t counter);
    const uint8_t* pixels() const { return frame_.data(); }

private:
    uint32_t width_;
    uint32_t height_;
    bool noise_;
    std::vector<uint8_t> background_;
    std::vector<uint8_t> frame_;
    std::vector<TestPatternRegion> changes_;
    std::optional<TestPatternRegion> previous_stripe_;
};

// Reads the counter from a BGRA image that contains the pattern scaled into
// the rectangle (x, y, w, h). Returns nullopt when the CRC does not match,
// for example while a transition is still blurred by the encoder.
std::optional<uint32_t> decode_test_pattern(const uint8_t* bgra, size_t stride,
                                            uint32_t image_width, uint32_t image_height,
                                            double x, double y, double w, double h);

}
