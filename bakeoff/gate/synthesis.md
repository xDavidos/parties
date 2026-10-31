# Synthesis: viewer queue feedback (implementation contract)

Base: candidate 1 (`tmp/architect/candidate-1/design.md`). Grafts and cuts below override it. Where this file is silent, candidate 1's design governs.

## Why candidate 1 is the base

- Charging bytes and timestamping them happen in one call (`SessionTransportStats::charge_video(bytes, now_us)`), so no send path can charge without marking. Candidate 2 relied on `forward_video_frame` remembering to call `on_sent`.
- An injected clock keeps `VideoSendAccounting::admit_frame()` unchanged, so `client/src/windows/app.cpp` and `client/src/macos/app_macos.mm` need no edit. macOS cannot be compiled on this machine.

Both candidates converged on: per-connection clock in `common/video_policy`, sojourn of the oldest unacknowledged byte minus MinRtt, never under-report when full, outstanding derived from monotonic charged/retired counters, a 4-byte `[VIDEO_CONTROL_TYPE][0x05][u16 LE ms]` report with the subtype as version and trailing bytes tolerated, worst viewer drives the sharer, gate unchanged as backstop, gate drop log to INFO. No cross-judge ran: the configured judge pool is Opus-only and the parent is Opus.

## Grafts and cuts

1. **Sharer report as a refusal deadline (from candidate 2).** Replace candidate 1's `PackedViewerReport`. `on_viewer_queue(std::chrono::milliseconds q)` runs on the network thread: fetch-max `q` into the window peak atomic; compute `excess = q - viewer_queue_budget_ms`; store `viewer_refusal_until_us = excess <= 0 ? INT64_MIN : clock() + min(excess, viewer_report_horizon_ms) * 1000`. `admit_frame()` refuses for the viewer while `clock() < viewer_refusal_until_us`. This is exactly "reported minus elapsed above budget", computed once per report.
2. **No `ViewerQueueDelay` type.** Use `std::chrono::milliseconds` everywhere above the wire codec. `ViewerQueueClock::delay` returns `std::chrono::milliseconds` saturated to 65535 ms by the codec.
3. **No config renames.** Keep `queue_budget_ms` and `congestion_queue_ms` as they are. Add `viewer_queue_budget_ms = 60`, `viewer_congestion_queue_ms = 40`, `viewer_report_horizon_ms = 500`.
4. **No separate reporter class.** The server uses a `Coalescer{50'000}` per sharer, as candidate 1 does: at most one report per sharer per 50 ms, sent when that sharer's frames arrive and it has at least one subscribed viewer.
5. **Marks container.** `std::deque` capped at 256 with widen-newest-when-full is acceptable; a fixed ring is not required. Keep the never-under-report invariant and its test.
6. **Clock injection.** `VideoSendController(const VideoSendControllerConfig& = {}, int64_t (*clock_us)() = steady_clock_us)`. `steady_clock_us()` is a free function in the controller header/source. Existing callers keep compiling.

## Thresholds and behaviour (measured, do not retune)

- Viewer admission budget 60 ms, congestion 40 ms (bakeoff r5: zero freezes in all five scenarios). 30/20 throttled a clean link because QUIC ACK delay floors the signal near 25 ms.
- A viewer refusal counts as an admission drop, so the window is congested, as in the prototype.
- Sampling: the server reads each viewer's queue in the gate loop before forwarding the current frame (the prototype's sampling fix).

## Out of scope

Encoder overshoot (H.264 4K sends ~6x its target), simulcast or per-viewer quality, upstream main port (stream ids). Mention them in docs only where the feedback loop is described.
