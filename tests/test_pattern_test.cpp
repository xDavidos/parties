#include <client/test_pattern.h>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace parties::client;

namespace {

bool passed = true;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Test pattern: %s\n", message);
        passed = false;
    }
}

std::vector<uint8_t> place(const uint8_t* frame, uint32_t fw, uint32_t fh,
                           uint32_t iw, uint32_t ih, uint32_t ox, uint32_t oy,
                           uint32_t rw, uint32_t rh) {
    std::vector<uint8_t> image(static_cast<size_t>(iw) * ih * 4, 40);
    for (uint32_t y = 0; y < rh; ++y) {
        for (uint32_t x = 0; x < rw; ++x) {
            const uint32_t sx = x * fw / rw;
            const uint32_t sy = y * fh / rh;
            const uint8_t* s = frame + (static_cast<size_t>(sy) * fw + sx) * 4;
            uint8_t* d = image.data() + (static_cast<size_t>(oy + y) * iw + ox + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        }
    }
    return image;
}

}

int main() {
    TestPatternRenderer renderer(1280, 720);
    for (uint32_t counter : {0u, 1u, 2u, 255u, 4096u, 123456u, kTestPatternCounterMask}) {
        const uint8_t* frame = renderer.render(counter);
        const auto direct = decode_test_pattern(frame, renderer.stride(), 1280, 720, 0, 0, 1280, 720);
        check(direct && *direct == counter, "full-size frame decodes to its counter");

        const auto image = place(frame, 1280, 720, 900, 600, 30, 64, 840, 472);
        const auto scaled = decode_test_pattern(image.data(), 900 * 4, 900, 600, 30, 64, 840, 472);
        check(scaled && *scaled == counter, "letterboxed downscale decodes to its counter");
    }

    for (uint8_t level : {uint8_t{0}, uint8_t{255}}) {
        std::vector<uint8_t> flat(1280 * 720 * 4, level);
        check(!decode_test_pattern(flat.data(), 1280 * 4, 1280, 720, 0, 0, 1280, 720),
              "a flat black or white image does not decode");
    }

    check(!decode_test_pattern(renderer.render(7), renderer.stride(), 1280, 720, 0, 0, 2000, 720),
          "a rectangle outside the image is rejected");

    // Applying only the reported changes must reproduce every full frame.
    for (const bool noise : {false, true}) {
    TestPatternRenderer incremental(1280, 720, noise);
    std::vector<uint8_t> target(1280 * 720 * 4, 0);
    for (uint32_t counter = 0; counter < 300; ++counter) {
        for (const TestPatternRegion& region : incremental.render_changes(counter)) {
            for (uint32_t y = region.y0; y < region.y1; ++y) {
                const size_t offset = (static_cast<size_t>(y) * 1280 + region.x0) * 4;
                std::copy_n(incremental.pixels() + offset, (region.x1 - region.x0) * 4, target.data() + offset);
            }
        }
        if (counter % 37 != 0) continue;
        TestPatternRenderer fresh(1280, 720, noise);
        const uint8_t* full = fresh.render(counter);
        if (!std::equal(target.begin(), target.end(), full)) {
            check(false, "incremental regions reproduce the full frame");
            break;
        }
        const auto decoded = decode_test_pattern(target.data(), 1280 * 4, 1280, 720, 0, 0, 1280, 720);
        check(decoded && *decoded == counter, "the counter survives the noise region");
    }
    }

    if (passed) std::printf("Test pattern: all checks passed\n");
    return passed ? 0 : 1;
}
