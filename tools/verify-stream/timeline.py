"""Bucketed timeline of one verification run: shows stalls that averages hide.

Usage: python -I timeline.py <evidence-dir> [--bucket-ms 500] [--freeze-ms 250]

All logs share one clock (QPC in 100 ns units). Per bucket it reports frames
reaching glass, the longest glass gap, glass-to-glass latency, frames the
viewer never received (the server's per-viewer backlog gate dropped them),
keyframes the sender emitted, and, when link.jsonl exists, the shaped link's
throughput, queue depth and dropped packets. Writes timeline.md next to the
other evidence.
"""
import argparse
import json
import statistics
from pathlib import Path

TICKS_PER_MS = 10_000


def events(path):
    if not path.exists():
        return []
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("evidence")
    p.add_argument("--bucket-ms", type=float, default=500)
    p.add_argument("--freeze-ms", type=float, default=250)
    p.add_argument("--warmup", type=float, default=2.0)
    a = p.parse_args()
    ev = Path(a.evidence)

    sender = events(ev / "sender-events.jsonl")
    viewer = events(ev / "viewer-events.jsonl")
    # A glass sample whose counter failed its CRC carries counter null.
    glass = [e for e in events(ev / "viewer-glass.jsonl")
             if e.get("ev") == "glass" and e.get("counter") is not None]
    link = [e for e in events(ev / "link.jsonl") if e.get("ev") == "link"]
    link_start = next((e for e in events(ev / "link.jsonl") if e.get("ev") == "link_start"), None)
    if not glass:
        raise SystemExit("no glass samples")

    generated = {}
    for e in sender:
        if e.get("ev") == "pattern" and e["counter"] not in generated:
            generated[e["counter"]] = e["t"]
    sent = [e for e in sender if e.get("ev") == "sent" and e.get("sent", True)]
    received = {e["seq"] for e in viewer if e.get("ev") == "recv"}

    # First composition of each new counter.
    shown = []
    best = -1
    for e in sorted(glass, key=lambda e: e["t"]):
        if e["counter"] > best:
            best = e["counter"]
            shown.append((e["t"], e["counter"]))

    t0 = shown[0][0] + int(a.warmup * 1000 * TICKS_PER_MS)
    t_end = glass[-1]["t"]
    shown = [s for s in shown if s[0] >= t0]
    bucket = int(a.bucket_ms * TICKS_PER_MS)
    n = max(1, (t_end - t0) // bucket + 1)
    rows = [dict(new=0, gap=0.0, g2g=[], sent=0, lost=0, keys=0, out=0, backlog=0.0, drops=0, link=False)
            for _ in range(n)]

    def idx(t):
        i = (t - t0) // bucket
        return i if 0 <= i < n else None

    prev = None
    for t, c in shown:
        i = idx(t)
        if i is None:
            continue
        rows[i]["new"] += 1
        if prev is not None:
            rows[i]["gap"] = max(rows[i]["gap"], (t - prev) / TICKS_PER_MS)
        if c in generated:
            rows[i]["g2g"].append((t - generated[c]) / TICKS_PER_MS)
        prev = t
    for e in sent:
        i = idx(e["t"])
        if i is None:
            continue
        rows[i]["sent"] += 1
        if e["seq"] not in received:
            rows[i]["lost"] += 1
        if e.get("key"):
            rows[i]["keys"] += 1
    for e in link:
        i = idx(e["t"])
        if i is None:
            continue
        r = rows[i]
        r["link"] = True
        r["out"] += e["out_bytes"]
        r["backlog"] = max(r["backlog"], e["backlog_ms"])
        r["drops"] += e["drop_pkts"]

    # Freeze episodes: gaps between consecutive new counters on glass.
    freezes = []
    for (ta, _), (tb, _) in zip(shown, shown[1:]):
        gap = (tb - ta) / TICKS_PER_MS
        if gap >= a.freeze_ms:
            freezes.append(((ta - t0) / TICKS_PER_MS / 1000, gap))
    starts = [f[0] for f in freezes]
    periods = [b - a_ for a_, b in zip(starts, starts[1:])]

    lines = [f"# Timeline {ev.name}", ""]
    if link_start:
        lines.append(f"Link toward the viewer: {link_start['down_mbps']:g} Mbps, "
                     f"{link_start['delay_ms']:g} ms each way, queue {link_start['buffer_bytes'] // 1024} KB.")
    total_sent = sum(r["sent"] for r in rows)
    total_lost = sum(r["lost"] for r in rows)
    lines += [
        f"Window {(t_end - t0) / TICKS_PER_MS / 1000:.1f} s after {a.warmup:g} s warm-up, "
        f"{a.bucket_ms:g} ms buckets.",
        f"Frames never received by the viewer (dropped by the server's backlog gate): "
        f"{total_lost} of {total_sent}.",
        f"Freezes (glass gap >= {a.freeze_ms:g} ms): {len(freezes)}"
        + (f", total {sum(f[1] for f in freezes) / 1000:.1f} s, longest {max(f[1] for f in freezes):.0f} ms"
           if freezes else "")
        + (f", median period {statistics.median(periods):.2f} s" if periods else "") + ".",
        "",
        "| t s | fps glass | max gap ms | g2g p50 ms | sent | gate-dropped | keyframes | link Mbps | queue ms | pkt drops |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    secs = a.bucket_ms / 1000
    for i, r in enumerate(rows):
        g2g = f"{statistics.median(r['g2g']):.0f}" if r["g2g"] else ""
        link_cols = (f"{r['out'] * 8 / secs / 1e6:.1f} | {r['backlog']:.0f} | {r['drops']}"
                     if r["link"] else " | | ")
        lines.append(f"| {i * secs:.1f} | {r['new'] / secs:.0f} | {r['gap']:.0f} | {g2g} | {r['sent']} | "
                     f"{r['lost']} | {r['keys']} | {link_cols} |")
    if freezes:
        lines += ["", "Freezes: " + ", ".join(f"{s:.1f} s ({g:.0f} ms)" for s, g in freezes)]
    text = "\n".join(lines) + "\n"
    (ev / "timeline.md").write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
