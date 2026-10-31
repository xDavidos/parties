#include <parties/viewer_queue.h>
#include <parties/protocol.h>

#include <chrono>
#include <cstdint>
#include <iostream>

using parties::MinRtt;
using parties::ViewerQueueClock;
using std::chrono::milliseconds;
namespace protocol = parties::protocol;

namespace {

constexpr int64_t T0 = 5'000'000;
constexpr int64_t MS = 1000;
constexpr MinRtt RTT_50MS = MinRtt::from_quic_min_rtt_us(50'000);

bool fail(const char* msg) {
    std::cerr << msg << "\n";
    return false;
}

bool test_nothing_sent() {
    ViewerQueueClock clock;
    if (clock.delay(0, RTT_50MS, T0) != milliseconds{0}) return fail("empty: nothing sent must read 0");
    return true;
}

bool test_unacked_frame_ages() {
    ViewerQueueClock clock;
    clock.on_sent(40'000, T0);
    if (clock.delay(0, RTT_50MS, T0 + 200 * MS) != milliseconds{150})
        return fail("age: frame unacked for 200 ms over a 50 ms MinRtt must read 150 ms");
    return true;
}

bool test_lone_small_frame_is_not_inflated() {
    ViewerQueueClock clock;
    clock.on_sent(300, T0);
    if (clock.delay(0, RTT_50MS, T0 + 55 * MS) != milliseconds{5})
        return fail("app-limited: a lone 300 B frame in flight for MinRtt + 5 ms must read 5 ms");
    return true;
}

bool test_retirement_advances_to_next_frame() {
    ViewerQueueClock clock;
    clock.on_sent(1000, T0);
    clock.on_sent(1500, T0 + 16 * MS);
    const int64_t now = T0 + 100 * MS;
    if (clock.delay(999, RTT_50MS, now) != milliseconds{50})
        return fail("retire: one byte short of the first frame must still date from the first frame");
    if (clock.delay(1000, RTT_50MS, now) != milliseconds{34})
        return fail("retire: retiring the first frame must date from the second frame");
    if (clock.delay(1500, RTT_50MS, now) != milliseconds{0})
        return fail("retire: everything retired must read 0");
    return true;
}

bool test_floor_and_saturation() {
    ViewerQueueClock clock;
    clock.on_sent(1000, T0);
    if (clock.delay(0, RTT_50MS, T0 + 20 * MS) != milliseconds{0})
        return fail("floor: a sojourn below MinRtt must read 0");
    const milliseconds long_wait = clock.delay(0, RTT_50MS, T0 + 70'000 * MS);
    const auto wire = protocol::encode_viewer_queue(long_wait);
    const auto decoded = protocol::decode_viewer_queue(wire.data(), wire.size());
    if (!decoded || *decoded != milliseconds{65'535})
        return fail("saturation: a 70 s sojourn must reach the wire as 65535 ms");
    return true;
}

bool test_overflow_never_under_reports() {
    ViewerQueueClock clock;
    for (int i = 0; i < 1000; ++i)
        clock.on_sent(static_cast<uint64_t>(i + 1) * 100, T0 + i * MS);
    const int64_t now = T0 + 2000 * MS;
    if (clock.delay(0, RTT_50MS, now) != milliseconds{1950})
        return fail("overflow: unretired sends must still date from the first send");
    for (uint64_t frames_acked : {100u, 254u, 255u, 600u, 999u}) {
        const int64_t true_sent_at = T0 + static_cast<int64_t>(frames_acked) * MS;
        const milliseconds true_delay{(now - true_sent_at - 50 * MS) / 1000};
        if (clock.delay(frames_acked * 100, RTT_50MS, now) < true_delay)
            return fail("overflow: the clock under-reported a sojourn");
    }
    if (clock.delay(1000 * 100, RTT_50MS, now) != milliseconds{0})
        return fail("overflow: everything retired must read 0");
    return true;
}

bool test_delay_is_idempotent() {
    ViewerQueueClock clock;
    clock.on_sent(1000, T0);
    clock.on_sent(2000, T0 + 10 * MS);
    const auto first = clock.delay(1000, RTT_50MS, T0 + 90 * MS);
    const auto second = clock.delay(1000, RTT_50MS, T0 + 90 * MS);
    if (first != second || first != milliseconds{30})
        return fail("idempotent: equal arguments must return equal delays");
    return true;
}

bool test_codec() {
    const auto wire = protocol::encode_viewer_queue(milliseconds{0x1234});
    if (wire.size() != protocol::VIEWER_QUEUE_REPORT_BYTES || wire[0] != protocol::VIDEO_CONTROL_TYPE ||
        wire[1] != static_cast<uint8_t>(protocol::VideoControl::ViewerQueue) ||
        wire[2] != 0x34 || wire[3] != 0x12)
        return fail("codec: layout must be [type][subtype][u16 LE]");
    const auto decoded = protocol::decode_viewer_queue(wire.data(), wire.size());
    if (!decoded || *decoded != milliseconds{0x1234}) return fail("codec: round trip");
    if (protocol::decode_viewer_queue(wire.data(), 3)) return fail("codec: 3-byte payload must be rejected");
    auto pli = wire;
    pli[1] = static_cast<uint8_t>(protocol::VideoControl::Pli);
    if (protocol::decode_viewer_queue(pli.data(), pli.size())) return fail("codec: wrong subtype must be rejected");
    const uint8_t longer[] = {wire[0], wire[1], wire[2], wire[3], 0x07, 0x09};
    const auto tolerant = protocol::decode_viewer_queue(longer, sizeof(longer));
    if (!tolerant || *tolerant != milliseconds{0x1234}) return fail("codec: trailing bytes must be ignored");
    const auto negative = protocol::encode_viewer_queue(milliseconds{-5});
    if (protocol::decode_viewer_queue(negative.data(), negative.size()) != milliseconds{0})
        return fail("codec: a negative queue must encode as 0");
    if (protocol::video_control_name(protocol::VideoControl::ViewerQueue) != "ViewerQueue")
        return fail("codec: subtype name");
    return true;
}

}

int main() {
    if (!test_nothing_sent()) return 1;
    if (!test_unacked_frame_ages()) return 1;
    if (!test_lone_small_frame_is_not_inflated()) return 1;
    if (!test_retirement_advances_to_next_frame()) return 1;
    if (!test_floor_and_saturation()) return 1;
    if (!test_overflow_never_under_reports()) return 1;
    if (!test_delay_is_idempotent()) return 1;
    if (!test_codec()) return 1;

    std::cout << "Viewer queue clock and ViewerQueue codec passed\n";
    return 0;
}
