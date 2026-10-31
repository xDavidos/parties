Steady state after 10 s. Pass: freezes (gap >= 250 ms) <= 0, g2g p95 <= 150 ms, glass fps >= 40, and not throttled (sent >= 0.6 x link or >= 0.9 x target).

| run | codec | target | link | rtt | window s | fps | freezes | freeze s | max gap ms | g2g p50 | g2g p95 | sent Mbps | recv Mbps | link use | never recv | keys | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| bo-p2-v5-E | H.265 | 12 | 8 | 80 | 35 | 5.9 | 54 | 30.7 | 604 | 91 | 157 | 0.2 | 0.2 | 2% | 0/211 | 0 | FAIL throttled |

1 runs, 0 errors, 206 steady-state frames on glass counted.
