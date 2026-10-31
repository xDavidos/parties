Steady state after 10 s. Pass: freezes (gap >= 250 ms) <= 0, g2g p95 <= 150 ms, glass fps >= 40, and not throttled (sent >= 0.6 x link or >= 0.9 x target).

| run | codec | target | link | rtt | window s | fps | freezes | freeze s | max gap ms | g2g p50 | g2g p95 | sent Mbps | recv Mbps | link use | never recv | keys | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bo-r6b30c20-v7-A |  |  | 10 | 20 | 35 | 27.9 | 11 | 9.5 | 3701 | 56 | 179 | 1.5 | 1.1 | 11% | 417/1730 | 15 | FAIL |
| bo-r6b30c20-v7-C | H.264 | 4 | 20 | 20 | 35 | 13.2 | 23 | 16.5 | 3410 | 336 | 1848 | 16.5 | 6.1 | 31% | 886/1382 | 27 | FAIL |
| bo-r6b30c20-v7-D | H.265 | 12 | 12.5 | 20 | 50 | 52.7 | 0 | 0.0 | 49 | 61 | 70 | 5.8 | 5.8 | 47% | 0/3001 | 10 | FAIL throttled |
| bo-r6b30c20-v7-E |  |  | 8 | 80 | 35 | 10.0 | 14 | 20.3 | 3854 | 138 | 1882 | 3.0 | 0.9 | 11% | 1006/1573 | 32 | FAIL |
| bo-r6b30c20-v7-B | H.265 | 12 | 40 | 20 | 20 | 16.2 | 13 | 11.5 | 2444 | 62 | 656 | 6.3 | 3.5 | 9% | 429/950 | 11 | FAIL throttled |

5 runs, 0 errors, 4744 steady-state frames on glass counted.
