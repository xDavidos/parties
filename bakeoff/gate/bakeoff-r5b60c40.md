Steady state after 10 s. Pass: freezes (gap >= 250 ms) <= 0, g2g p95 <= 150 ms, glass fps >= 40, and not throttled (sent >= 0.6 x link or >= 0.9 x target).

| run | codec | target | link | rtt | window s | fps | freezes | freeze s | max gap ms | g2g p50 | g2g p95 | sent Mbps | recv Mbps | link use | never recv | keys | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bo-r5b60c40-v7-A | H.265 | 12 | 10 | 20 | 35 | 54.7 | 0 | 0.0 | 69 | 65 | 91 | 7.0 | 7.0 | 70% | 0/2099 | 7 | PASS |
| bo-r5b60c40-v7-C | H.264 | 4 | 20 | 20 | 35 | 40.2 | 0 | 0.0 | 139 | 125 | 176 | 18.5 | 18.5 | 93% | 2/1421 | 5 | FAIL |
| bo-r5b60c40-v7-D | H.265 | 12 | 12.5 | 20 | 50 | 53.7 | 0 | 0.0 | 118 | 65 | 90 | 8.5 | 8.5 | 68% | 0/2981 | 10 | PASS |
| bo-r5b60c40-v7-E | H.265 | 12 | 8 | 80 | 35 | 51.4 | 0 | 0.0 | 174 | 97 | 151 | 5.9 | 5.9 | 73% | 0/1998 | 7 | FAIL |
| bo-r5b60c40-v7-B | H.265 | 12 | 40 | 20 | 20 | 48.7 | 0 | 0.0 | 49 | 58 | 67 | 13.2 | 13.2 | 33% | 0/1199 | 4 | PASS |

5 runs, 0 errors, 8775 steady-state frames on glass counted.
