# Shaped-link additions for the verify-parties-stream skill

The skill itself lives in the git-ignored `.claude/skills/verify-parties-stream/`.
These files extend it so a run can put the viewer behind a bandwidth-limited link
and show stalls over time instead of only run averages.

| File | Purpose |
|---|---|
| `link_shaper.py` | User-space UDP relay between the viewer and the server. Server-to-viewer traffic crosses a bottleneck of `--down-mbps` behind a drop-tail queue (default 100 ms at the rate), and both directions take `--delay-ms`. No driver or elevation is needed. It logs throughput, queue depth and drops every 100 ms to `link.jsonl` on the clients' QPC clock. |
| `timeline.py` | Buckets one run's evidence (default 500 ms) into frames on glass, the longest glass gap, glass-to-glass latency, frames the server's backlog gate dropped (sent but never received), keyframes, and the shaped link's state. Lists freezes and their period. Writes `timeline.md`. |
| `verify-stream-viewer-link.patch` | Adds `-ViewerLinkMbps`, `-ViewerLinkDelayMs` and `-ViewerLinkBufferKB` to `verify-stream.ps1`. With a link set, `launch` starts the shaper, the viewer joins through it, and the server logs at debug level so the gate's drop lines are kept. It also waits up to 3 s for the mute and deafen buttons to show `active`, because RmlUi applies bound classes on the next render. |

Install into a checkout's skill:

```powershell
$skill = '.claude/skills/verify-parties-stream/scripts'
Copy-Item tools/verify-stream/link_shaper.py, tools/verify-stream/timeline.py $skill
git apply --directory=.claude/skills/verify-parties-stream tools/verify-stream/verify-stream-viewer-link.patch
```

Example: a 4K stream through a 10 Mbps viewer link with 10 ms each way.

```powershell
pwsh -File .claude/skills/verify-parties-stream/scripts/verify-stream.ps1 run -Run bw1 -PatternSize 3840x2160 -Noise -Codec H.265 -BitrateMbps 12 -Seconds 60 -ViewerLinkMbps 10 -ViewerLinkDelayMs 10
python -I .claude/skills/verify-parties-stream/scripts/timeline.py build/verify-evidence/bw1 --bucket-ms 1000
```
