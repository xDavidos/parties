Steady state after 10 s. Pass: freezes (gap >= 250 ms) <= 0, g2g p95 <= 150 ms, glass fps >= 40.

| run | codec | target | link | rtt | window s | fps | freezes | freeze s | max gap ms | g2g p50 | g2g p95 | sent Mbps | recv Mbps | never recv | keys | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bo-r1-off,v1,v2,v3,v4-A | H.265 | 12 | 10 | 20 | 35 | 29.7 | 14 | 4.3 | 361 | 260 | 412 | 13.3 | 8.4 | 802/2131 | 45 | FAIL |
| bo-r1-off,v1,v2,v3,v4-C | H.264 | 4 | 20 | 20 | 35 | 39.5 | 16 | 4.7 | 354 | 264 | 418 | 25.3 | 17.1 | 721/2108 | 37 | FAIL |
| bo-r1-off,v1,v2,v3,v4-D | H.265 | 12 | 12.5 | 20 | 50 | 40.8 | 4 | 1.3 | 368 | 222 | 316 | 13.1 | 11.6 | 355/3007 | 31 | FAIL |
| bo-r1-off,v1,v2,v3,v4-E | H.265 | 12 | 8 | 80 | 35 | 12.2 | 44 | 14.7 | 583 | 319 | 801 | 13.3 | 4.6 | 1381/2128 | 54 | FAIL |
| bo-r1-off,v1,v2,v3,v4-B | H.265 | 12 | 40 | 20 | 20 | 42.0 | 0 | 0.0 | 97 | 60 | 93 | 11.7 | 11.7 | 0/1066 | 4 | PASS |

5 runs, 0 errors, 5730 steady-state frames on glass counted.
