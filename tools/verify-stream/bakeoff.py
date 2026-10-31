"""Steady-state comparison of verification runs, one row per run.

Usage: python -I bakeoff.py <evidence-dir> [<evidence-dir> ...] [--settle 10] [--freeze-ms 250]
                            [--max-freezes 0] [--max-g2g-p95-ms 150] [--min-fps 40] [--out FILE]

The settle window skips the first seconds after the first frame reaches glass, so a
controller that needs a few seconds to find the link is judged on where it lands.
Every metric comes from the same QPC-stamped logs that timeline.py reads.
"""
import argparse
import json
import statistics
import sys
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


def pct(values, q):
    if not values:
        return None
    s = sorted(values)
    return s[min(len(s) - 1, int(q * (len(s) - 1) + 0.5))]


def measure(ev, settle_s, freeze_ms):
    sender = events(ev / "sender-events.jsonl")
    viewer = events(ev / "viewer-events.jsonl")
    glass = [e for e in events(ev / "viewer-glass.jsonl")
             if e.get("ev") == "glass" and e.get("counter") is not None]
    link_start = next((e for e in events(ev / "link.jsonl") if e.get("ev") == "link_start"), None)
    if not glass:
        return {"run": ev.name, "error": "no glass samples"}

    generated = {}
    for e in sender:
        if e.get("ev") == "pattern" and e["counter"] not in generated:
            generated[e["counter"]] = e["t"]

    shown = []
    best = -1
    for e in sorted(glass, key=lambda e: e["t"]):
        if e["counter"] > best:
            best = e["counter"]
            shown.append((e["t"], e["counter"]))

    t0 = shown[0][0] + int(settle_s * 1000 * TICKS_PER_MS)
    t_end = glass[-1]["t"]
    window_s = (t_end - t0) / TICKS_PER_MS / 1000
    if window_s < 5:
        return {"run": ev.name, "error": f"steady window only {window_s:.1f} s"}

    steady = [s for s in shown if t0 <= s[0] <= t_end]
    gaps = [(b[0] - a[0]) / TICKS_PER_MS for a, b in zip(steady, steady[1:])]
    freezes = [g for g in gaps if g >= freeze_ms]
    g2g = [(t - generated[c]) / TICKS_PER_MS for t, c in steady if c in generated]

    received = {e["seq"] for e in viewer if e.get("ev") == "recv"}
    sent = [e for e in sender if e.get("ev") == "sent" and e.get("sent", True) and t0 <= e["t"] <= t_end]
    sent_bytes = sum(e["bytes"] for e in sent)
    never_received = sum(1 for e in sent if e["seq"] not in received)
    recv_bytes = sum(e["bytes"] for e in viewer if e.get("ev") == "recv" and t0 <= e["t"] <= t_end)
    keys = sum(1 for e in sent if e.get("key"))

    report = {}
    if (ev / "report.json").exists():
        report = json.loads((ev / "report.json").read_text(encoding="utf-8"))
    req = report.get("requested", {})

    return {
        "run": ev.name,
        "codec": req.get("codec", ""),
        "target_mbps": req.get("bitrate_mbps"),
        "link_mbps": link_start["down_mbps"] if link_start else None,
        "rtt_ms": 2 * link_start["delay_ms"] if link_start else None,
        "window_s": window_s,
        "frames_on_glass": len(steady),
        "fps": len(steady) / window_s,
        "freezes": len(freezes),
        "freeze_s": sum(freezes) / 1000,
        "max_gap_ms": max(gaps) if gaps else 0.0,
        "g2g_p50": pct(g2g, 0.50),
        "g2g_p95": pct(g2g, 0.95),
        "sent_mbps": sent_bytes * 8 / window_s / 1e6,
        "recv_mbps": recv_bytes * 8 / window_s / 1e6,
        "frames_sent": len(sent),
        "never_received": never_received,
        "keyframes": keys,
    }


def fmt(v, spec):
    return "" if v is None else format(v, spec)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("evidence", nargs="+")
    p.add_argument("--settle", type=float, default=10.0)
    p.add_argument("--freeze-ms", type=float, default=250)
    p.add_argument("--max-freezes", type=int, default=0)
    p.add_argument("--max-g2g-p95-ms", type=float, default=150)
    p.add_argument("--min-fps", type=float, default=40)
    p.add_argument("--min-link-use", type=float, default=0.6,
                   help="fail a shaped run that sent less than this share of the link "
                        "while also sending under 90%% of its target")
    p.add_argument("--out")
    a = p.parse_args()

    rows = [measure(Path(e), a.settle, a.freeze_ms) for e in a.evidence]
    lines = [
        f"Steady state after {a.settle:g} s. Pass: freezes (gap >= {a.freeze_ms:g} ms) <= {a.max_freezes}, "
        f"g2g p95 <= {a.max_g2g_p95_ms:g} ms, glass fps >= {a.min_fps:g}, and not throttled "
        f"(sent >= {a.min_link_use:g} x link or >= 0.9 x target).",
        "",
        "| run | codec | target | link | rtt | window s | fps | freezes | freeze s | max gap ms "
        "| g2g p50 | g2g p95 | sent Mbps | recv Mbps | link use | never recv | keys | verdict |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    errors = 0
    for r in rows:
        if "error" in r:
            errors += 1
            lines.append(f"| {r['run']} | | | | | | | | | | | | | | | | | ERROR {r['error']} |")
            continue
        link, target = r["link_mbps"], r["target_mbps"]
        throttled = (link is not None and target is not None
                     and r["sent_mbps"] < a.min_link_use * link
                     and r["sent_mbps"] < 0.9 * target)
        link_use = f"{r['recv_mbps'] / link:.0%}" if link else ""
        passed = (r["freezes"] <= a.max_freezes
                  and r["g2g_p95"] is not None and r["g2g_p95"] <= a.max_g2g_p95_ms
                  and r["fps"] >= a.min_fps
                  and not throttled)
        lines.append(
            f"| {r['run']} | {r['codec']} | {fmt(r['target_mbps'], 'g')} | {fmt(r['link_mbps'], 'g')} "
            f"| {fmt(r['rtt_ms'], 'g')} | {r['window_s']:.0f} | {r['fps']:.1f} | {r['freezes']} "
            f"| {r['freeze_s']:.1f} | {r['max_gap_ms']:.0f} | {fmt(r['g2g_p50'], '.0f')} "
            f"| {fmt(r['g2g_p95'], '.0f')} | {r['sent_mbps']:.1f} | {r['recv_mbps']:.1f} | {link_use} "
            f"| {r['never_received']}/{r['frames_sent']} | {r['keyframes']} "
            f"| {'PASS' if passed else ('FAIL throttled' if throttled else 'FAIL')} |")
    counted = sum(r.get("frames_on_glass", 0) for r in rows)
    lines += ["", f"{len(rows)} runs, {errors} errors, {counted} steady-state frames on glass counted."]
    text = "\n".join(lines) + "\n"
    if a.out:
        Path(a.out).write_text(text, encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    print(text)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
