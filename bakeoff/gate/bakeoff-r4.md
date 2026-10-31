Steady state after 10 s. Pass: freezes (gap >= 250 ms) <= 0, g2g p95 <= 150 ms, glass fps >= 40, and not throttled (sent >= 0.6 x link or >= 0.9 x target).

| run | codec | target | link | rtt | window s | fps | freezes | freeze s | max gap ms | g2g p50 | g2g p95 | sent Mbps | recv Mbps | link use | never recv | keys | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bo-r4-v7-A | H.265 | 12 | 10 | 20 | 35 | 49.6 | 2 | 0.7 | 389 | 70 | 137 | 7.8 | 7.6 | 76% | 37/1985 | 9 | FAIL |
| bo-r4-v7-C | H.264 | 4 | 20 | 20 | 35 | 42.3 | 0 | 0.0 | 215 | 172 | 247 | 19.3 | 19.3 | 96% | 2/1505 | 5 | FAIL |
| bo-r4-v7-D | H.265 | 12 | 12.5 | 20 | 50 | 50.5 | 2 | 0.5 | 264 | 69 | 130 | 9.6 | 9.6 | 77% | 0/2917 | 12 | FAIL |
| bo-r4-v7-E | H.265 | 12 | 8 | 80 | 35 | 50.3 | 1 | 0.4 | 382 | 98 | 162 | 5.8 | 5.7 | 71% | 27/2026 | 8 | FAIL |
| bo-r4-v7-B |  |  | 40 | 20 | 20 | 6.1 | 8 | 5.7 | 1604 | 95 | 627 | 9.5 | 1.4 | 3% | 640/958 | 21 | FAIL |
| bo-r4-v8-A | | | | | | | | | | | | | | | | | ERROR no glass samples |
| bo-r4-v8-C | H.264 | 4 | 20 | 20 | 35 | 17.0 | 10 | 16.8 | 11201 | 191 | 742 | 19.2 | 8.2 | 41% | 926/1566 | 30 | FAIL |
| bo-r4-v8-D | H.265 | 12 | 12.5 | 20 | 50 | 51.7 | 1 | 0.3 | 271 | 69 | 98 | 9.7 | 9.7 | 77% | 0/2961 | 11 | FAIL |
| bo-r4-v8-E | H.265 | 12 | 8 | 80 | 35 | 54.9 | 0 | 0.0 | 49 | 89 | 98 | 3.0 | 3.0 | 37% | 0/2098 | 7 | FAIL throttled |
| bo-r4-v8-B | H.265 | 12 | 40 | 20 | 20 | 48.5 | 0 | 0.0 | 49 | 58 | 67 | 13.3 | 13.3 | 33% | 0/1200 | 4 | PASS |

10 runs, 1 errors, 13689 steady-state frames on glass counted.
