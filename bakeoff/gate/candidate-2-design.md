# Viewer queue feedback: design (candidate 2)

The server timestamps every video byte boundary it sends to each viewer, reads the age of the oldest unacknowledged byte minus that viewer's MinRtt, and tells each sharer the worst queue among the sharer's viewers in a 4-byte `VideoControl::ViewerQueue` datagram. The sharer turns each report into a refusal deadline. Frames captured before the deadline are not encoded. The window peak of the reports feeds AIMD. The gate stays as the backstop.

## Usage

### Server relay, `Server::forward_video_frame` (main loop)

```cpp
std::chrono::milliseconds worst{0};
for (auto& s : quic_.get_sessions()) {
    if (/* not a subscribed viewer of sharer */) continue;
    const auto& t = s->transport_stats;
    worst = std::max(worst, s->video_queue.delay(t.video_retired_bytes(), t.min_rtt(), now));
    switch (gate.admit(t.video_outstanding_bytes(), threshold, keyframe)) { /* unchanged */ }
}
for (auto& s : targets) {
    if (quic_.send_video_frame_to(s, shared_frame))
        s->video_queue.on_sent(s->transport_stats.video_sent_bytes(), now);
    else { /* unchanged: require_keyframe, want_keyframe, ++dropped */ }
}
if (auto report = st.queue_report.offer(worst, now)) {
    const auto dg = protocol::encode_viewer_queue_report(*report);
    quic_.send_datagram(sharer_session.id, dg.data(), dg.size(), DatagramPriority::AheadOfVideo);
}
```

### Sharer network thread, `AppCore::init` datagram callback

```cpp
} else if (type == protocol::VIDEO_CONTROL_TYPE) {
    if (auto queue = protocol::parse_viewer_queue_report(data, len))
        video_sender_.accounting().on_viewer_queue(*queue, steady_now_us());
    else if (/* existing Pli branch, unchanged */) bridge_.request_keyframe();
}
```

### Sharer capture thread (Windows `app.cpp`, macOS `app_macos.mm`)

```cpp
if (!core_.video_accounting().admit_frame(steady_now_us())) { log_pattern("admission"); return; }
```

The main thread keeps calling `video_sender_.tick(steady_now_us())`. Nothing else on the sharer changes.

## Type sketch

### `common/include/parties/protocol.h`

```cpp
enum class VideoControl : uint8_t {
    Pli               = 0x01,
    ReservedShareStart = 0x02,
    ReservedShareStop  = 0x03,
    ReservedNack       = 0x04,
    ViewerQueue        = 0x05,   // was ReservedReceiverReport
};

// server -> sharer datagram: [VIDEO_CONTROL_TYPE][0x05][queue_ms u16 LE]
// queue_ms: worst queueing delay among the sharer's viewers, saturated at 65535.
// Receivers accept len >= 4 and ignore trailing bytes; new fields may only be appended.
constexpr size_t VIEWER_QUEUE_REPORT_BYTES = 4;

constexpr std::array<uint8_t, VIEWER_QUEUE_REPORT_BYTES>
encode_viewer_queue_report(std::chrono::milliseconds queue);       // not implemented

constexpr std::optional<std::chrono::milliseconds>
parse_viewer_queue_report(const uint8_t* datagram, size_t len);   // not implemented
// nullopt unless len >= 4, data[0] == VIDEO_CONTROL_TYPE, data[1] == ViewerQueue
```

The codec sits beside the enum, so the server and the client share one definition of the layout. Callers above the codec handle only `std::chrono::milliseconds`.

### `common/video_policy/include/parties/viewer_queue.h` (new)

```cpp
namespace parties {

// How long video bytes wait in front of one viewer's bottleneck.
// One per viewer connection, covering every sharer that viewer watches, because
// all of them share the viewer's downlink. Main-loop only, like ViewerVideoGate.
class ViewerQueueClock {
public:
    static constexpr size_t MAX_MARKS = 256;

    // sent_bytes_total: the connection's cumulative charged video bytes right after a frame was sent.
    void on_sent(uint64_t sent_bytes_total, int64_t now_us);   // not implemented

    // Age of the oldest unacknowledged byte minus min_rtt, floored at 0. 0 when nothing is in flight.
    // retired_bytes_total: the connection's cumulative acknowledged-or-abandoned video bytes.
    [[nodiscard]] std::chrono::milliseconds delay(uint64_t retired_bytes_total, MinRtt min_rtt,
                                                  int64_t now_us);   // not implemented

private:
    struct Mark { uint64_t end_bytes; int64_t sent_at_us; };
    std::deque<Mark> marks_;   // end_bytes strictly increasing
};

// Decides when a sharer gets a ViewerQueue report. One per sharer, main loop only.
class ViewerQueueReporter {
public:
    static constexpr int64_t MIN_INTERVAL_US = 50'000;

    // The worst viewer queue measured for this sharer's latest frame.
    // Returns the value to send now, or nullopt. A healthy link with no viewer queue sends nothing:
    // one zero follows the last nonzero report and then the reporter goes quiet.
    [[nodiscard]] std::optional<std::chrono::milliseconds>
    offer(std::chrono::milliseconds worst, int64_t now_us);   // not implemented

private:
    std::optional<int64_t> last_sent_us_;
    bool last_sent_nonzero_ = false;
};

}
```

Pseudocode:

```text
on_sent(total, now):
    if marks_ non-empty and total <= marks_.back().end_bytes: return     // send failed or nothing charged
    if marks_.size() == MAX_MARKS: marks_.back().end_bytes = total       // widen the newest mark, keep its older time:
    else: marks_.push_back({total, now})                                 // the head age is never underestimated

delay(retired, min_rtt, now):
    while marks_ non-empty and marks_.front().end_bytes <= retired: pop_front
    if empty: return 0
    age_us = now - marks_.front().sent_at_us
    return floor_ms(max(0, age_us - min_rtt.us()))

offer(worst, now):
    if worst == 0 and not last_sent_nonzero_: return nullopt
    if last_sent_us_ and now - *last_sent_us_ < MIN_INTERVAL_US: return nullopt
    last_sent_us_ = now; last_sent_nonzero_ = worst > 0
    return worst
```

`delay()` pops marks, but the same inputs give the same answer, so a repeated call is harmless.

### `common/video_policy/include/parties/video_send_controller.h`

```cpp
struct VideoSendControllerConfig {
    // ... existing fields unchanged (queue_budget_ms and congestion_queue_ms stay the sharer's own uplink) ...
    uint32_t viewer_queue_budget_ms    = 60;    // reported queue, drained by elapsed time, above this refuses frames
    uint32_t viewer_congestion_ms      = 40;    // window peak of raw reports at or above this is a congested window
    uint32_t viewer_refusal_horizon_ms = 500;   // one report never refuses for longer than this
};

namespace video_send_detail {
struct SharedCounters {
    // ... existing ...
    std::atomic<uint32_t> viewer_queue_budget_ms{60};      // written only by start_share
    std::atomic<uint32_t> viewer_refusal_horizon_ms{500};  // written only by start_share
    std::atomic<int64_t>  viewer_refusal_until_us{std::numeric_limits<int64_t>::min()};
    std::atomic<uint32_t> viewer_queue_peak_ms{0};
    std::atomic<uint32_t> viewer_refusals{0};
};
}

class VideoSendAccounting {
public:
    // ... on_bytes_queued, on_bytes_retired, on_send_failed, bitrate_bps unchanged ...

    // Network thread. The newest report replaces any earlier one.
    void on_viewer_queue(std::chrono::milliseconds worst_viewer_queue, int64_t received_at_us);  // not implemented

    // Capture thread. Refuses while the sharer's own queue exceeds its allowance or while the
    // latest viewer report, drained by the time since it arrived, still exceeds the viewer budget.
    [[nodiscard]] bool admit_frame(int64_t now_us);   // signature change: was admit_frame()
};

struct VideoSendController::Stats {   // existing fields plus:
    uint32_t viewer_refusals = 0;     // subset of admission_drops refused for a viewer queue
};
```

Pseudocode:

```text
on_viewer_queue(q, at):
    q_ms = clamp(q.count(), 0, UINT32_MAX)
    fetch_max(viewer_queue_peak_ms, q_ms)
    excess = q_ms - viewer_queue_budget_ms
    until = excess <= 0 ? INT64_MIN : at + min(excess, viewer_refusal_horizon_ms) * 1000
    viewer_refusal_until_us.store(until)

admit_frame(now):
    own_ok    = outstanding <= allowance_bytes
    viewer_ok = now >= viewer_refusal_until_us
    if own_ok and viewer_ok: return true
    if not viewer_ok: ++viewer_refusals
    ++admission_drops; ++admission_drops_window       // a refused frame still marks the window congested
    return false

tick, at window end:  congested |= viewer_queue_peak_ms.exchange(0) >= cfg_.viewer_congestion_ms
start_share:          mirror the two viewer config fields, reset viewer_refusal_until_us, viewer_queue_peak_ms, viewer_refusals
on_connection_lost:   via start_share, as today
```

`now >= until` is the drained-report rule written once: `reported - (now - at) > budget` is the same as `now < at + (reported - budget)`. The network thread does that arithmetic once per report. Then the capture thread only needs one compare.

### `common/video_policy/include/parties/video_common.h`

```cpp
int64_t steady_now_us();   // replaces the file-local copies in app_core.cpp and windows/app.cpp
```

### `server/include/server/session.h`

```cpp
class SessionTransportStats {
public:
    uint64_t video_sent_bytes() const;        // cumulative, monotonic
    uint64_t video_retired_bytes() const;     // cumulative, monotonic, never above sent
    int64_t  video_outstanding_bytes() const; // derived: loads retired (acquire) then sent, returns the difference
    // min_rtt(), alive() unchanged
private:
    friend class QuicServer;
    friend class SessionTransport;
    void charge_video(uint32_t bytes);   // video_sent_bytes_ += bytes
    void retire_video(uint32_t bytes);   // video_retired_bytes_ += bytes (release)
    std::atomic<uint64_t> video_sent_bytes_{0};
    std::atomic<uint64_t> video_retired_bytes_{0};
    // video_outstanding_bytes_ is deleted
};

struct Session {
    // ...
    std::unordered_map<UserId, ViewerVideoGate> video_gates;   // main loop only (existing)
    ViewerQueueClock video_queue;                               // main loop only (new)
};
```

The three existing uncharge sites (`SendCtx::uncharge`, `VideoOutStreamContext::uncharge`, and the `SendFailed` path in `SessionTransport::send_on_new_frame_stream`) call `retire_video`. The two charge sites call `charge_video`. Outstanding bytes are no longer stored. They are the difference of two counters, so the gate and the clock always read the same byte ledger.

### `server/include/server/server.h`

```cpp
struct SharerVideoState {
    // ... existing ...
    ViewerQueueReporter queue_report;
};
```

The gate's drop log in `forward_video_frame` moves from `LOG_DEBUG` to `LOG_INFO` and gains the worst viewer queue. The existing 1 s per-sharer coalescer still limits it. Now that feedback works, a gate firing means the loop failed, and an operator needs to see that at the default log level.

## Module map

| File | Change |
|---|---|
| `common/include/parties/protocol.h` | `ViewerQueue = 0x05`, `video_control_name`, codec |
| `common/video_policy/include/parties/viewer_queue.h`, `src/viewer_queue.cpp` | new `ViewerQueueClock`, `ViewerQueueReporter` |
| `common/video_policy/CMakeLists.txt` | add `src/viewer_queue.cpp` |
| `common/video_policy/include/parties/video_send_controller.h`, `src/video_send_controller.cpp` | viewer config, `on_viewer_queue`, `admit_frame(now_us)`, window peak, `viewer_refusals` |
| `common/video_policy/include/parties/video_common.h`, `src/video_common.cpp` | `steady_now_us()` |
| `server/include/server/session.h` | sent and retired counters, outstanding derived, `video_queue` |
| `server/src/session_transport.h`, `server/src/quic_server.cpp` | charge and retire through the two private methods |
| `server/include/server/server.h`, `server/src/server.cpp` | reporter per sharer, worst queue in the forward loop, `on_sent` after a successful send, gate log at INFO |
| `client/src/app_core.cpp` | parse `ViewerQueue`, call `on_viewer_queue`, use shared `steady_now_us` |
| `client/src/windows/app.cpp`, `client/src/macos/app_macos.mm` | `admit_frame(steady_now_us())`, drop the file-local clock |
| `tests/viewer_queue_test.cpp` (new), `tests/CMakeLists.txt` | clock, reporter, and codec tests |
| `tests/video_send_controller_test.cpp` | viewer-report behaviour, `admit_frame(t)` at existing sites |
| `docs/protocol.md`, `docs/video-pipeline.md` | the subtype, the feedback loop, the gate as backstop |

Trace: viewer ACK → MsQuic `SEND_COMPLETE` → `retire_video` (atomic) → the next sharer frame on the main loop → `ViewerQueueClock::delay` → max over viewers → `ViewerQueueReporter::offer` → datagram → sharer network thread `on_viewer_queue` → atomic deadline → capture thread `admit_frame`. Across threads, the data passes only through atomics that already have one writer each.

## Behaviour

- **Multi-viewer:** the worst subscribed viewer decides the report. That includes a viewer whose gate is dropping right now. Viewers on good links get the slowest viewer's quality at full smoothness.
- **Multi-sharer:** a viewer has one clock, so it reports the same queue to every sharer it watches. Each of those sharers backs off. The reports stop once the shared downlink drains.
- **Stale reports:** no timer is needed. The deadline expires by itself, so a sharer that refuses frames and therefore triggers no new reports admits a frame again after at most `min(reported - budget, horizon)`.
- **Old peers:** old sharers ignore subtype 0x05 because they only match `Pli`. Old servers send no reports, so a new sharer behaves exactly as it does today. The server ignores 0x05 from clients because `handle_video_control` only matches `Pli`. No protocol version bump.

## Test plan (behaviour, not internals)

`tests/viewer_queue_test.cpp`
1. Nothing sent gives 0 delay.
2. Frames sent at t0 and t0+20 ms, nothing acked, at t0+100 ms with MinRtt 30 ms gives 70 ms. After the first frame's bytes retire it gives 50 ms. After all bytes retire it gives 0.
3. Retiring part of a frame keeps that frame's age.
4. A sojourn shorter than MinRtt gives 0, never a negative value.
5. After more than `MAX_MARKS` sends with nothing acked, the delay equals the age of the very first send (the bound never underestimates).
6. A failed send (the total does not grow) leaves the delay unchanged.
7. Reporter: zero offers on a fresh reporter send nothing. A nonzero value is sent, then nothing until 50 ms have passed. The first zero after a nonzero value is sent, and later zeros are not.
8. Codec: values round-trip. 70 000 ms saturates to 65535. A short datagram, another subtype, or another type gives nullopt. Trailing bytes are accepted.

`tests/video_send_controller_test.cpp`
1. A good link with no reports: frames are admitted and the bitrate climbs to the ceiling as today, with existing tests passing after changing only `admit_frame()` to `admit_frame(t)`.
2. A 200 ms report at t refuses at t and at t+139 ms and admits at t+141 ms with a 60 ms budget.
3. A 60 ms report admits right away.
4. A zero report after a 200 ms report admits right away.
5. A 5000 ms report admits again at t+500 ms because of the horizon.
6. Reports peaking at 40 ms in two consecutive windows cut the bitrate by `decrease_factor`. Reports peaking at 39 ms leave the AIMD increasing.
7. `viewer_refusals` counts only viewer refusals. Own-queue refusals still count in `admission_drops` only.
8. `start_share` and `on_connection_lost` clear a pending refusal.

End to end: run the `verify-parties-stream` bakeoff scenarios A to E with the predicate from the grounding. The gate drop log should appear rarely or not at all in A, B, and D.
