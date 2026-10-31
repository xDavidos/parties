Steady state after 10 s. Pass: freezes (gap >= 250 ms) <= 0, g2g p95 <= 150 ms, glass fps >= 40, and not throttled (sent >= 0.6 x link or >= 0.9 x target).

| run | codec | target | link | rtt | window s | fps | freezes | freeze s | max gap ms | g2g p50 | g2g p95 | sent Mbps | recv Mbps | link use | never recv | keys | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bo-r3-v5-A | H.265 | 12 | 10 | 20 | 35 | 53.3 | 1 | 0.5 | 486 | 64 | 96 | 6.6 | 6.6 | 66% | 1/2065 | 6 | FAIL |
| bo-r3-v5-C | H.264 | 4 | 20 | 20 | 35 | 5.9 | 51 | 29.8 | 931 | 96 | 160 | 3.2 | 2.5 | 13% | 53/264 | 6 | FAIL throttled |
| bo-r3-v5-D | H.265 | 12 | 12.5 | 20 | 50 | 48.7 | 7 | 3.7 | 597 | 65 | 90 | 7.8 | 7.8 | 63% | 0/2752 | 10 | FAIL |
| bo-r3-v5-E | H.265 | 12 | 8 | 80 | 35 | 6.1 | 55 | 31.3 | 604 | 90 | 152 | 0.2 | 0.2 | 2% | 0/226 | 1 | FAIL throttled |
| bo-r3-v5-B | H.265 | 12 | 40 | 20 | 20 | 48.4 | 0 | 0.0 | 49 | 58 | 67 | 13.2 | 13.3 | 33% | 0/1199 | 4 | PASS |
| bo-r3-v6-A | H.265 | 12 | 10 | 20 | 35 | 52.9 | 0 | 0.0 | 63 | 65 | 91 | 7.4 | 7.4 | 74% | 0/2102 | 7 | PASS |
| bo-r3-v6-C | H.264 | 4 | 20 | 20 | 35 | 6.3 | 54 | 30.4 | 632 | 84 | 145 | 2.7 | 2.6 | 13% | 4/226 | 2 | FAIL throttled |
| bo-r3-v6-D | H.265 | 12 | 12.5 | 20 | 50 | 52.0 | 2 | 0.9 | 486 | 61 | 82 | 5.9 | 5.9 | 47% | 0/2937 | 10 | FAIL throttled |
| bo-r3-v6-E | H.265 | 12 | 8 | 80 | 35 | 6.0 | 55 | 31.0 | 597 | 93 | 152 | 0.2 | 0.2 | 2% | 0/217 | 1 | FAIL throttled |
| bo-r3-v6-B | H.265 | 12 | 40 | 20 | 20 | 54.2 | 0 | 0.0 | 42 | 53 | 63 | 7.3 | 7.3 | 18% | 0/1201 | 4 | FAIL throttled |

10 runs, 0 errors, 11662 steady-state frames on glass counted.
