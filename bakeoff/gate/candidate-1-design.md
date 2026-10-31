# Viewer queue feedback: design (candidate 1)

Base: `verify-stream/hillclimb`. Nothing from `proto/gate-bakeoff` is copied; its env switch, variants v1-v6 and v8, the delivered-rate and backlog-byte fields, and the 512-entry pop-front deque all die.

## Usage first

### Server, `Server::forward_video_frame` (main loop)

```cpp
auto& st = sharer_video_state_[sharer];
const bool report_due = st.queue_report.allow(now);          // Coalescer{50'000}
ViewerQueueDelay worst = ViewerQueueDelay::none();
for (auto& s : quic_.get_sessions()) {
    if (/* same viewer filter as today */) continue;
    if (report_due) worst = std::max(worst, s->transport_stats.video_queue_delay(now));
    // gate exactly as today
}
for (auto& s : targets) quic_.send_video_frame_to(s, shared_frame);   // charges AND marks
if (report_due) send_viewer_queue(sharer_session, worst);
```

The server never touches marks, retired counters or MinRtt. `video_queue_delay(now)` is the only call needed, and `send_video_frame_to` is the only place bytes enter a viewer's queue, so it is also the only place they are timestamped.

### Sharer, `AppCore::init` datagram callback (network thread)

```cpp
} else if (type == protocol::VIDEO_CONTROL_TYPE && len >= 2) {
    switch (static_cast<protocol::VideoControl>(data[1])) {
    case protocol::VideoControl::Pli: /* unchanged */ break;
    case protocol::VideoControl::ViewerQueue:
        if (auto ms = protocol::decode_viewer_queue(data, len))
            video_sender_.accounting().on_viewer_queue(ViewerQueueDelay::from_ms(*ms));
        break;
    default: break;
    }
}
```

### Sharer, capture thread (Windows `app.cpp:1455`, macOS `app_macos.mm:959`): unchanged

```cpp
if (!core_.video_accounting().admit_frame()) { log_pattern("admission"); return; }
```

`admit_frame()` keeps its signature. It now also refuses while the worst viewer's drained queue is over budget. Platform code does not change and cannot pass the wrong clock.

### Tests

```cpp
int64_t g_now = T0;
VideoSendController c(cfg, [] { return g_now; });
c.accounting().on_viewer_queue(ViewerQueueDelay::from_ms(150));
expect(!c.accounting().admit_frame());          // 150 > 60 budget
g_now += 89 * MS;  expect(!c.accounting().admit_frame());   // 150-89 = 61
g_now += 1 * MS;   expect(c.accounting().admit_frame());    // drained to 60
```

## Wire format

Server to sharer, priority QUIC datagram (`DatagramPriority::AheadOfVideo`), same plane as the PLI:

```
+------+------+-------------------+
| 0x03 | 0x05 | queue_ms (u16 LE) |   4 bytes
+------+------+-------------------+
```

- `queue_ms`: max over this sharer's current viewers of (age of the viewer's oldest unacknowledged video byte - viewer MinRtt), clamped to [0, 65535]. 0 with no viewers.
- No stream id, no version byte. The subtype is the version: an incompatible meaning takes a new subtype. Decoders require at least 4 bytes and ignore trailing bytes, so a port to upstream main may append `stream_id (u8)` without breaking old sharers (they apply it to their one video controller, which is the conservative failure).
- No protocol version bump. Old clients ignore unknown subtypes (verified in `AppCore::init`); old servers never send it. The server's `handle_video_control` handles only `Pli` from clients, so a client cannot forge one toward another sharer.
- Rate: at most one per sharer per 50 ms, and only when that sharer's frames arrive.

## Type sketch

### `common/include/parties/protocol.h`

```cpp
enum class VideoControl : uint8_t {
    Pli = 0x01, ReservedShareStart = 0x02, ReservedShareStop = 0x03, ReservedNack = 0x04,
    ViewerQueue = 0x05,   // server -> sharer only; was ReservedReceiverReport
};
// video_control_name: "ViewerQueue"

inline constexpr size_t VIEWER_QUEUE_REPORT_BYTES = 4;
std::array<uint8_t, VIEWER_QUEUE_REPORT_BYTES> encode_viewer_queue(uint16_t queue_ms);
// Accepts [VIDEO_CONTROL_TYPE][ViewerQueue][u16 LE ...]; nullopt when shorter or mistyped.
std::optional<uint16_t> decode_viewer_queue(const uint8_t* data, size_t len);
```

The byte layout exists only in these two functions. Server and client both call them.

### `common/video_policy/include/parties/viewer_queue.h` (new)

```cpp
// How long video has waited in one viewer's downlink queue beyond the path's floor.
// It is a time, never bytes over a rate: rate-normalised estimates inflate when the
// sharer is app-limited (bakeoff v1/v5/v6). Saturates at 65535 ms.
class ViewerQueueDelay {
public:
    static constexpr ViewerQueueDelay none() { return ViewerQueueDelay{0}; }
    static constexpr ViewerQueueDelay from_ms(uint32_t ms);                      // saturating
    static constexpr ViewerQueueDelay from_sojourn(int64_t oldest_unacked_age_us, MinRtt);
    constexpr uint16_t ms() const { return ms_; }
    friend constexpr auto operator<=>(ViewerQueueDelay, ViewerQueueDelay) = default;
private:
    constexpr explicit ViewerQueueDelay(uint16_t ms) : ms_(ms) {}
    uint16_t ms_;
};

// Sojourn of the oldest unacknowledged byte on one viewer connection, from two
// monotonic byte positions. Single-threaded; owner calls both methods.
// Invariant: it may over-report a sojourn, never under-report one.
class ViewerQueueClock {
public:
    static constexpr size_t MAX_MARKS = 256;   // ~4 s of unacked 60 fps frames

    // Bytes up to `sent_total` (cumulative bytes charged to this viewer) left at `now_us`.
    // A position at or behind the newest mark is ignored. When full, the newest mark
    // absorbs the position, keeping its earlier time (over-reports, never under-reports).
    void on_sent(uint64_t sent_total, int64_t now_us);

    // Drops marks fully covered by `retired_total` and returns
    // from_sojourn(now - time of the first uncovered mark, min_rtt), none() when empty.
    // Idempotent for equal arguments.
    ViewerQueueDelay delay(uint64_t retired_total, MinRtt min_rtt, int64_t now_us);

private:
    struct Mark { uint64_t sent_total; int64_t sent_at_us; };
    std::array<Mark, MAX_MARKS> ring_{};
    size_t head_ = 0;
    size_t size_ = 0;
};
```

`viewer_queue.cpp` holds the ring logic. `MinRtt` stays in `video_backlog.h`, which `viewer_queue.h` includes.

### `server/include/server/session.h`: `SessionTransportStats`

```cpp
// Written by the transport. Thread roles: charge_video and video_queue_delay run on the
// server loop only; retire_video runs on any MsQuic thread.
class SessionTransportStats {
public:
    int64_t video_outstanding_bytes() const;        // charged - retired, derived, never stored
    ViewerQueueDelay video_queue_delay(int64_t now_us);   // server loop only
    MinRtt min_rtt() const;
    bool alive() const;
private:
    friend class QuicServer;
    friend class SessionTransport;
    void charge_video(uint32_t bytes, int64_t now_us);    // charged += bytes; clock.on_sent(charged, now)
    void retire_video(uint32_t bytes);                    // retired += bytes
    std::atomic<uint64_t> video_charged_bytes_{0};
    std::atomic<uint64_t> video_retired_bytes_{0};
    ViewerQueueClock      video_queue_clock_;             // server loop only
    std::atomic<uint32_t> min_rtt_us_{VIDEO_BACKLOG_DEFAULT_RTT_US};
    std::atomic<bool>     alive_{true};
};
```

`video_outstanding_bytes_` (the signed counter) is deleted. Every site that did `fetch_add` now calls `charge_video`, every `fetch_sub` calls `retire_video`: `SendCtx::uncharge`, `VideoOutStreamContext::uncharge`, the `StreamSend` failure path in `SessionTransport::send_on_new_frame_stream`, and the legacy charge in `QuicServer::send_video_frame_to`. `send_on_new_frame_stream` gains an `int64_t now_us` parameter.

Pseudocode:

```cpp
int64_t video_outstanding_bytes() const {
    const uint64_t retired = video_retired_bytes_.load(acquire);   // load first: retired <= charged
    const uint64_t charged = video_charged_bytes_.load(relaxed);   // written by this thread only
    return charged >= retired ? int64_t(charged - retired) : 0;
}
ViewerQueueDelay video_queue_delay(int64_t now_us) {
    return video_queue_clock_.delay(video_retired_bytes_.load(acquire), min_rtt(), now_us);
}
```

### `common/video_policy/include/parties/video_send_controller.h`

```cpp
using VideoClockUs = int64_t (*)();
int64_t steady_clock_us();

struct VideoSendControllerConfig {
    uint32_t ceiling_bitrate_bps        = VIDEO_DEFAULT_BITRATE;
    uint32_t floor_bitrate_bps          = VIDEO_ADAPT_MIN_BITRATE;
    uint32_t uplink_queue_budget_ms     = 120;   // renamed from queue_budget_ms
    uint32_t uplink_congestion_queue_ms = 80;    // renamed from congestion_queue_ms
    uint32_t viewer_queue_budget_ms     = 60;    // refuse frames while drained worst-viewer queue exceeds
    uint32_t viewer_congestion_queue_ms = 40;    // window congested when a report reaches this
    uint32_t viewer_report_ttl_ms       = 500;   // older reports never refuse
    // window_ms ... min_reconfigure_ratio unchanged
};

namespace video_send_detail {
// One 64-bit atomic so a reader never pairs a new queue with an old receipt time:
// (received_at_ms << 16) | queue_ms; 0 means no report.
class PackedViewerReport { /* pack(queue, at_us) / queue() / received_at_us() */ };

struct SharedCounters {
    explicit SharedCounters(VideoClockUs clock) : clock(clock) {}
    const VideoClockUs clock;
    // ... existing atomics ...
    std::atomic<uint64_t> viewer_report{0};
    std::atomic<uint32_t> viewer_peak_ms_window{0};
    std::atomic<uint32_t> viewer_budget_ms{60};
    std::atomic<uint32_t> viewer_ttl_ms{500};
    std::atomic<uint32_t> viewer_admission_drops{0};
};
}

class VideoSendAccounting {
public:
    // existing: on_bytes_queued, on_bytes_retired, on_send_failed, bitrate_bps
    void on_viewer_queue(ViewerQueueDelay worst);   // network thread; stamps with the clock
    [[nodiscard]] bool admit_frame();               // capture thread; also honours the viewer queue
};

class VideoSendController {
public:
    explicit VideoSendController(const VideoSendControllerConfig& cfg = {},
                                 VideoClockUs clock = steady_clock_us);
    // start_share, on_connection_lost, set_ceiling_bitrate, on_path_counters, tick: unchanged signatures
    struct Stats {
        // existing fields ...
        uint32_t viewer_queue_ms        = 0;   // drained worst-viewer queue at stats() time
        uint32_t viewer_admission_drops = 0;   // subset of admission_drops
    };
};
```

Pseudocode, accounting:

```cpp
void on_viewer_queue(ViewerQueueDelay q) {
    c_.viewer_report.store(PackedViewerReport::pack(q, c_.clock()).bits(), release);
    atomic_fetch_max(c_.viewer_peak_ms_window, q.ms());
}
uint32_t drained_viewer_queue_ms(const SharedCounters& c, int64_t now_us) {
    auto r = PackedViewerReport{c.viewer_report.load(acquire)};
    if (r.empty()) return 0;
    const int64_t elapsed_ms = (now_us - r.received_at_us()) / 1000;
    if (elapsed_ms >= c.viewer_ttl_ms) return 0;
    return max<int64_t>(0, r.queue().ms() - elapsed_ms);
}
bool admit_frame() {
    const bool uplink_ok = outstanding <= allowance;
    const bool viewer_ok = drained_viewer_queue_ms(c_, c_.clock()) <= c_.viewer_budget_ms;
    if (uplink_ok && viewer_ok) return true;
    if (!viewer_ok) ++viewer_admission_drops;
    ++admission_drops; ++admission_drops_window;   // either refusal is congestion, as in v7
    return false;
}
```

`tick`, at window close: `congested |= viewer_peak_ms_window.exchange(0) >= cfg_.viewer_congestion_queue_ms`. `start_share` and `on_connection_lost` clear `viewer_report`, `viewer_peak_ms_window`, `viewer_admission_drops`, and publish the budget and TTL atomics.

### Server additions, `server.h` / `server.cpp`

```cpp
struct SharerVideoState { /* existing */ Coalescer queue_report{50'000}; };
void send_viewer_queue(const Session& sharer, ViewerQueueDelay worst);  // encode + send_datagram AheadOfVideo
```

## Gate and drop log

`ViewerVideoGate`, `viewer_backlog_threshold_bytes` and the PLI suppression in `handle_video_control` stay byte-for-byte. They read `video_outstanding_bytes()`, which is now derived. The gate is the backstop and should rarely fire. The DEBUG-only drop log becomes `LOG_INFO`, still coalesced to one line per sharer per second, reworded as "backstop dropped N viewer-frame(s)" and extended with the last reported worst viewer queue. When the feedback works, it stays silent. When it prints, the feedback failed, so operators should see it.

## Multi-viewer and multi-sharer

- Many viewers of one sharer: the report is the max over that sharer's subscribed viewers, so the slowest viewer sets the pace for all (grounding default). The policy lives in one `std::max` line in `forward_video_frame`.
- One viewer watching sharers A and B: one connection, one clock. Its queue reaches both A and B, and both back off. The clock is per connection, not per (sharer, viewer), because the queue being measured is per connection.
- Several sharers: independent `SharerVideoState::queue_report` coalescers, so each sharer gets its own reports.
- A viewer's clock lives and dies with its `Session`. Reconnect starts at zero. Subscribe and unsubscribe need no clock maintenance.

## Module map

| File | Change |
|---|---|
| `common/include/parties/protocol.h` | `ViewerQueue = 0x05`, name, `encode_viewer_queue` / `decode_viewer_queue` |
| `common/video_policy/include/parties/viewer_queue.h`, `src/viewer_queue.cpp` (new) | `ViewerQueueDelay`, `ViewerQueueClock` |
| `common/video_policy/CMakeLists.txt` | add `viewer_queue.cpp` |
| `common/video_policy/include/parties/video_send_controller.h`, `src/video_send_controller.cpp` | config rename + viewer fields, clock injection, `on_viewer_queue`, refusal, window peak, stats |
| `server/include/server/session.h` | charged/retired counters, derived outstanding, clock, `video_queue_delay` |
| `server/src/session_transport.h`, `server/src/quic_server.cpp` | charge/retire through the two private methods; `now_us` into frame-stream send |
| `server/include/server/server.h`, `server/src/server.cpp` | `queue_report` coalescer, worst-viewer max, `send_viewer_queue`, drop log to INFO |
| `client/src/app_core.cpp` | datagram callback `ViewerQueue` branch; `begin_video_share` uses renamed config fields if it sets them |
| `tests/viewer_queue_test.cpp` (new), `tests/video_send_controller_test.cpp`, `tests/CMakeLists.txt` | see test plan |
| `docs/protocol.md`, `docs/video-pipeline.md` | VIEWER_QUEUE row and layout, sharer admission row, gate described as backstop |
| `client/src/windows/app.cpp`, `client/src/macos/app_macos.mm` | no change |

## Test plan (behaviour, not internals)

`tests/viewer_queue_test.cpp`:
1. Nothing sent: `delay` is `none()`.
2. One frame sent at t, unacked, sampled at t+200 ms, MinRtt 50 ms: 150 ms.
3. Lone small frame in flight for MinRtt + 5 ms reads 5 ms, whatever the bitrate. This is the regression test for lesson 1 (app-limited inflation).
4. Two frames at t and t+16 ms. Retiring exactly the first frame's bytes moves the sojourn to the second frame's send time. Retiring one byte less does not.
5. Sojourn below MinRtt reads 0, never negative. A sojourn over 65.535 s saturates.
6. 1000 sends without retirement: the reported sojourn still dates from the first send. Overflow never under-reports.
7. `delay` twice with equal arguments returns equal values.
8. Codec: encode/decode round trip, a 3-byte payload rejected, the wrong subtype rejected, trailing bytes tolerated.

`tests/video_send_controller_test.cpp` (fake clock):
1. Report above budget refuses; with no new report, admission resumes exactly when reported - elapsed reaches the budget (drain, lesson 2).
2. Report at budget admits.
3. A report older than the TTL never refuses, even if not drained.
4. Two windows whose peak report is 40 ms apply 0.8x. Peak 39 ms with no refusals leaves the bitrate alone, and clean windows still raise it.
5. Viewer refusals count in `viewer_admission_drops` and `admission_drops`, and make the window congested.
6. `start_share` forgets a pending report: the next frame is admitted.
7. Existing uplink tests pass with the renamed fields. No report means today's behaviour.

End to end: `verify-parties-stream` bakeoff scenarios A-E with no env switch must meet the grounding predicate (0 gaps >= 250 ms, p95 <= 150 ms except the documented C/E gaps, fps >= 40, >= 60% link), and the backstop INFO line must not print in B.
