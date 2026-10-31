# Grounding: viewer congestion feedback for Parties screen share

## User goal

A viewer whose downlink is narrower than the stream's real bitrate must see a smooth stream (no freezes, bounded latency) instead of today's cycle: stable, latency sawtooth, freeze to 0-2 fps, keyframe, repeat. Viewers on good links must not regress.

## How the system works today (code on branch `verify-stream/hillclimb`)

- Sharer client encodes frames and sends each on its own QUIC unidirectional stream to the server (`client/src/app_core.cpp` `send_video_frame`, `client/src/net_client_msquic.cpp`).
- Sharer rate control: `VideoSendController` in `common/video_policy/{include/parties/video_send_controller.h,src/video_send_controller.cpp}`. Three faces over one `video_send_detail::SharedCounters` block of atomics:
  - `VideoSendAccounting` (network and capture threads): `on_bytes_queued/retired`, `admit_frame()` called per captured frame on the capture thread (`client/src/windows/app.cpp:1455`, macOS equivalent). Refusing means the frame is never encoded, so no reference chain breaks.
  - `VideoEncodeFeedback` (encode thread): `take_bitrate_update()`, `take_keyframe_request()`.
  - `VideoSendController` itself (main thread): `tick(now_us)` every app tick, AIMD over 500 ms windows (x0.8 after 2 congested windows, x1.08 after 4 clean, floor 800 kbps), `on_path_counters()` from MsQuic stats.
  - Congestion today = own uplink only: own queue delay >= 80 ms, loss > 3%, admission drops, send failures.
- Server relay: `Server::forward_video_frame` (`server/src/server.cpp` ~1420-1520). Per incoming sharer frame it loops over viewers in the channel subscribed to that sharer, asks each viewer's `ViewerVideoGate` (`common/video_policy/include/parties/video_backlog.h`) whether to forward, and sends with `quic_.send_video_frame_to`.
  - Gate: when a viewer's `transport_stats.video_outstanding_bytes()` exceeds `viewer_rate x (MinRtt + 250 ms) + largest recent keyframe` (clamped 64 KB-2 MB), it drops that frame and every later delta until a keyframe; requests a keyframe from the sharer (PLI, coalesced 200 ms; sharer forces keyframes at most every 500 ms).
  - Outstanding bytes per viewer session: `SessionTransportStats` in `server/include/server/session.h`, charged on send and uncharged on SEND_COMPLETE in `server/src/quic_server.cpp` and `server/src/session_transport.h`. `SendBufferingEnabled = FALSE` (`server/src/quic_server.cpp:186`), so SEND_COMPLETE fires on ACK: outstanding includes about one RTT of in-flight data.
  - The server already sends the sharer PLIs as datagrams: `request_keyframe_from_sharer` builds `[VIDEO_CONTROL_TYPE][VideoControl::Pli][requester u32]` and calls `quic_.send_datagram(sharer->id, ..., DatagramPriority::AheadOfVideo)`. The sharer handles VIDEO_CONTROL_TYPE datagrams on a network thread in `AppCore::init`'s datagram callback (`client/src/app_core.cpp` ~238) and only understands `Pli`; other subtypes are ignored, so a new subtype is backward compatible both ways.
  - `protocol::VideoControl` (`common/include/parties/protocol.h:212`) reserves `ReservedReceiverReport = 0x05`.
- The sharer never learns about viewer congestion, so it keeps sending the same rate and the gate cycles. Reproduced with a shaped viewer link (tools/verify-stream/link_shaper.py).

## What the prototype bakeoff proved (branch `proto/gate-bakeoff`, evidence under build/verify-evidence/bo-*)

Scenarios, 4K noise test pattern, one viewer behind a shaped link: A H.265 12 Mbps into 10 Mbps; C H.264 4 Mbps target (encoder really sends ~24 Mbps) into 20 Mbps; D H.265 12 into 12.5; E H.265 12 into 8 Mbps with 80 ms RTT; B control H.265 12 into 40 Mbps. Predicate after a 10 s settle: 0 glass gaps >= 250 ms, g2g p95 <= 150 ms, fps >= 40, not throttled below 60% of the link.

| Variant | Signal | Result |
|---|---|---|
| off (today) | none | A C D E fail with 4-44 freezes, p95 316-801 ms |
| v4 | softer gate only | no better than off |
| v1 | viewer queue (outstanding / sharer rate) into AIMD | cannot fix C (encoder ignores bitrate cuts); over-throttles E |
| v2/v3 | v1 + sharer skips frames when viewer queue > budget (+ rate cap) | collapse to ~6 fps on C and E |
| v5/v6 | queue = outstanding / delivered rate - MinRtt | same collapse |
| v7 | queue = age of the oldest unacknowledged byte - MinRtt; sharer drains a reported queue by its age; AIMD + skipping | 0-2 freezes, 70-96% link use |
| v7 tuned (budget 60 ms, congestion 40 ms) | same | 0 freezes in all five; A D B pass; C p95 176, E p95 151 |

Two lessons the design must encode:
1. Any queue estimate normalised by a measured rate inflates when the sharer sends little (a lone in-flight frame over an app-limited rate reads as a frame interval of queue), and the sharer's own skipping drives the spiral. The signal must be a time: sojourn of the oldest unacknowledged byte per viewer, minus that viewer's MinRtt.
2. Reports are produced when sharer frames arrive. A sharer that refuses frames produces no new reports, so a reported queue must drain on the sharer by its age (queue_now = reported - elapsed), or refusal deadlocks on stale data.

Prototype sojourn tracker: per viewer, a deque of (cumulative bytes sent to that viewer after this frame, send time) appended after each successful `send_video_frame_to`; at report time pop marks whose cumulative <= the viewer's cumulative retired bytes; head age = now - front.time (0 when empty). Cumulative retired bytes = new `video_retired_bytes_` atomic incremented wherever outstanding is decremented.

Thresholds: viewer admission budget 60 ms, viewer congestion threshold 40 ms. 30/20 throttled a clean link to 47% because QUIC ACK delay (up to ~25 ms) puts a noise floor under the sojourn signal. The gate stays as a backstop and must rarely fire.

Known remaining gaps (not for this design to solve): scenario C's latency comes from the H.264 encoder ignoring its bitrate target at 4K, so frame skipping holds a standing queue at the budget; E's p95 sits at ~150 ms with 40 ms of its one-way delay built in.

## Constraints

- Threads: server forward path is the main loop (single writer of per-sharer state; no locks needed, see "gate is mutated only by its owner"). Client: report parsed on a network thread, `admit_frame` on the capture thread, `tick` on the main thread; handoff through atomics in `SharedCounters`.
- Multi-viewer default (user may override): the worst viewer drives the sharer (max queue over viewers), so every viewer stays smooth at the slowest viewer's quality.
- A viewer can watch several sharers; outstanding/retired counters are per viewer session, shared across sharers.
- Upstream main has diverged (stream ids for webcams, protocol 1.4). Keep the wire message small and self-describing so it ports.
- Repo rules: AGENTS.md (English only), no comments that restate code, typography rules irrelevant here. Unit tests live in `tests/` (`tests/video_send_controller_test.cpp`, `tests/video_backlog_test.cpp`); test behaviour, not internals. Protocol docs in `docs/protocol.md`, pipeline docs in `docs/video-pipeline.md`.
