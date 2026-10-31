#pragma once

#include <parties/video_backlog.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace parties {

// How long the oldest unacknowledged video byte on one viewer connection has
// waited beyond the path's MinRtt. It is a time on purpose: bytes divided by a
// measured rate inflate when the sharer sends little, and the sharer's own
// backing off then feeds the inflation.
// Not thread-safe: the owner calls both methods from one thread.
class ViewerQueueClock {
public:
    static constexpr size_t MAX_MARKS = 256;

    // `sent_total` is the cumulative byte count charged to this connection
    // after a send. When full, the newest mark absorbs the position and keeps
    // its earlier time, so the clock may over-report a sojourn but never
    // under-reports one.
    void on_sent(uint64_t sent_total, int64_t now_us);

    std::chrono::milliseconds delay(uint64_t retired_total, MinRtt min_rtt, int64_t now_us);

private:
    struct Mark {
        uint64_t sent_total;
        int64_t  sent_at_us;
    };
    std::deque<Mark> marks_;
};

}
