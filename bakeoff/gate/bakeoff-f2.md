Steady state after 10 s. Pass: freezes (gap >= 250 ms) <= 0, g2g p95 <= 150 ms, glass fps >= 40, and not throttled (sent >= 0.6 x link or >= 0.9 x target).

| run | codec | target | link | rtt | window s | fps | freezes | freeze s | max gap ms | g2g p50 | g2g p95 | sent Mbps | recv Mbps | link use | never recv | keys | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bo-f2-off-A | H.265 | 12 | 10 | 20 | 35 | 54.7 | 0 | 0.0 | 63 | 66 | 106 | 7.8 | 7.8 | 78% | 0/2067 | 7 | PASS |
| bo-f2-off-C | H.264 | 4 | 20 | 20 | 35 | 41.2 | 0 | 0.0 | 146 | 130 | 180 | 18.9 | 18.9 | 94% | 3/1458 | 5 | FAIL |
| bo-f2-off-D | H.265 | 12 | 12.5 | 20 | 50 | 52.1 | 0 | 0.0 | 125 | 68 | 92 | 9.5 | 9.5 | 76% | 0/2958 | 9 | PASS |
| bo-f2-off-E | H.265 | 12 | 8 | 80 | 35 | 50.9 | 0 | 0.0 | 208 | 97 | 145 | 5.5 | 5.5 | 68% | 2/2024 | 7 | PASS |
| bo-f2-off-B |  |  | 40 | 20 | 20 | 48.8 | 0 | 0.0 | 49 | 57 | 67 | 13.2 | 13.2 | 33% | 0/1202 | 4 | PASS |

5 runs, 0 errors, 8723 steady-state frames on glass counted.
