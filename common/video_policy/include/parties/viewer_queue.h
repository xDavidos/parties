#pragma once

#include <parties/video_backlog.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace parties {

class ViewerQueueClock {
public:
    static constexpr size_t MAX_MARKS = 256;

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
