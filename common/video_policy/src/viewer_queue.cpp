#include <parties/viewer_queue.h>

namespace parties {

void ViewerQueueClock::on_sent(uint64_t sent_total, int64_t now_us) {
    if (!marks_.empty() && sent_total <= marks_.back().sent_total) return;
    if (marks_.size() == MAX_MARKS) {
        marks_.back().sent_total = sent_total;
        return;
    }
    marks_.push_back({sent_total, now_us});
}

std::chrono::milliseconds ViewerQueueClock::delay(uint64_t retired_total, MinRtt min_rtt,
                                                  int64_t now_us) {
    while (!marks_.empty() && marks_.front().sent_total <= retired_total) marks_.pop_front();
    if (marks_.empty()) return std::chrono::milliseconds{0};
    const int64_t queued_us = now_us - marks_.front().sent_at_us - static_cast<int64_t>(min_rtt.us());
    if (queued_us <= 0) return std::chrono::milliseconds{0};
    return std::chrono::milliseconds{queued_us / 1000};
}

}
