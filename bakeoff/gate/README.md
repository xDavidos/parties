# Gate bakeoff audit trail

This folder records how the viewer queue feedback fix was chosen. `decisions.tsv` is the decision log, one row per decision, with evidence pointers.

Every table comes from `tools/verify-stream/bakeoff.py` over runs of the `verify-parties-stream` harness. The source is a 4K noise test pattern, and the viewer sits behind a shaped link. A run passes when, after a 10 s settle window, it has no glass gap of 250 ms or more, glass-to-glass p95 of at most 150 ms, at least 40 fps, and is not throttled below 60% of the link.

| Scenario | Stream | Viewer link |
|---|---|---|
| A | H.265, 12 Mbps target | 10 Mbps, 20 ms RTT |
| C | H.264, 4 Mbps target (the encoder sends about 24 Mbps) | 20 Mbps, 20 ms RTT |
| D | H.265, 12 Mbps target | 12.5 Mbps, 20 ms RTT |
| E | H.265, 12 Mbps target | 8 Mbps, 80 ms RTT |
| B (control) | H.265, 12 Mbps target | 40 Mbps, 20 ms RTT |

| Table | What ran |
|---|---|
| `bakeoff-r1.md` | Off mode on the prototype build. The run ids say `off,v1,v2,v3,v4` because of a driver bug, but every process ran off mode. |
| `bakeoff-r2.md` | v1 to v4 |
| `bakeoff-r3.md` | v5 and v6 |
| `bakeoff-p1.md`, `bakeoff-p2.md` | v5 on E with diagnostics, to test the premise |
| `bakeoff-p3.md` | v7 on E |
| `bakeoff-r4.md` | v7 and v8 |
| `bakeoff-r5b60c40.md` | v7 with a 60 ms budget and a 40 ms congestion threshold |
| `bakeoff-r6b30c20.md` | v7 with 30 ms and 20 ms. Inconclusive because of harness failures. |
| `bakeoff-f1.md`, `bakeoff-f2.md` | The production fix on branch `fix/viewer-queue-feedback`, two passes |

`matrix.ps1` is the driver that produced the tables. `grounding.md`, the two candidate designs, and `synthesis.md` are the architect inputs and the implementation contract.
