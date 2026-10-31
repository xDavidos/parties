Steady state after 10 s. Pass: freezes (gap >= 250 ms) <= 0, g2g p95 <= 150 ms, glass fps >= 40, and not throttled (sent >= 0.6 x link or >= 0.9 x target).

| run | codec | target | link | rtt | window s | fps | freezes | freeze s | max gap ms | g2g p50 | g2g p95 | sent Mbps | recv Mbps | link use | never recv | keys | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bo-f1-off-A | H.265 | 12 | 10 | 20 | 35 | 54.1 | 0 | 0.0 | 69 | 65 | 90 | 6.7 | 6.7 | 67% | 0/2100 | 7 | PASS |
| bo-f1-off-C | H.264 | 4 | 20 | 20 | 35 | 39.9 | 0 | 0.0 | 174 | 127 | 174 | 18.4 | 18.4 | 92% | 1/1438 | 5 | FAIL |
| bo-f1-off-D | H.265 | 12 | 12.5 | 20 | 50 | 51.3 | 0 | 0.0 | 146 | 68 | 110 | 9.2 | 9.2 | 74% | 0/2923 | 10 | PASS |
| bo-f1-off-E | H.265 | 12 | 8 | 80 | 35 | 50.6 | 0 | 0.0 | 167 | 98 | 147 | 5.6 | 5.6 | 70% | 1/2005 | 7 | PASS |
| bo-f1-off-B | H.265 | 12 | 40 | 20 | 20 | 48.5 | 0 | 0.0 | 62 | 57 | 67 | 13.2 | 13.2 | 33% | 0/1186 | 4 | PASS |

5 runs, 0 errors, 8594 steady-state frames on glass counted.
