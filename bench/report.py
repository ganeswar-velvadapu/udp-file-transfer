#!/usr/bin/env python3
"""Turn bench/results/results.csv into results.md + SVG charts (no dependencies).

usage: python3 bench/report.py [results.csv]
"""

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from xml.sax.saxutils import escape

# Categorical slots 1 and 2 of the reference palette (light / dark steps).
UDP_COLOR = ("#2a78d6", "#3987e5")
TCP_COLOR = ("#eb6834", "#d95926")

STYLE = f"""<style>
  svg {{ font-family: -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif; }}
  .bg {{ fill: #fcfcfb; }} .title {{ fill: #0b0b0b; font-size: 15px; font-weight: 600; }}
  .sub, .tick, .legend {{ fill: #52514e; font-size: 12px; }} .value {{ fill: #0b0b0b; font-size: 11px; }}
  .grid {{ stroke: #e4e3df; stroke-width: 1; }} .axis {{ stroke: #b5b4ae; stroke-width: 1; }}
  .udp {{ fill: {UDP_COLOR[0]}; }} .tcp {{ fill: {TCP_COLOR[0]}; }}
  .udp-line {{ stroke: {UDP_COLOR[0]}; }} .tcp-line {{ stroke: {TCP_COLOR[0]}; }}
  .ring {{ stroke: #fcfcfb; stroke-width: 2; }}
  @media (prefers-color-scheme: dark) {{
    .bg {{ fill: #1a1a19; }} .title, .value {{ fill: #f0efec; }} .sub, .tick, .legend {{ fill: #c3c2b7; }}
    .grid {{ stroke: #2f2f2d; }} .axis {{ stroke: #55544f; }}
    .udp {{ fill: {UDP_COLOR[1]}; }} .tcp {{ fill: {TCP_COLOR[1]}; }}
    .udp-line {{ stroke: {UDP_COLOR[1]}; }} .tcp-line {{ stroke: {TCP_COLOR[1]}; }}
    .ring {{ stroke: #1a1a19; }}
  }}
</style>"""

LABELS = {
    "clean": "no shaping",
    "rtt20": "RTT 20ms",
    "rtt20_loss1": "+1% loss",
    "rtt20_loss5": "+5% loss",
    "rtt20_loss10": "+10% loss",
    "rtt20_dup_reorder": "2% loss, dup, reorder",
}


def nice_max(v):
    if v <= 0:
        return 1
    import math
    exp = 10 ** math.floor(math.log10(v))
    for m in (1, 2, 2.5, 5, 10):
        if v <= m * exp:
            return m * exp
    return 10 * exp


def fmt(v):
    if v == int(v) and v >= 10 or v == 0:
        return f"{v:.0f}"
    if v >= 100:
        return f"{v:.0f}"
    if v >= 10:
        return f"{v:.1f}"
    return f"{v:.2f}"


def bar_path(x, y0, w, h, r=4):
    """Bar anchored at baseline y0 growing up by h, rounded only at the data end."""
    if h <= 0:
        return ""
    r = min(r, w / 2, h)
    top = y0 - h
    return (f"M{x:.1f},{y0:.1f} V{top + r:.1f} Q{x:.1f},{top:.1f} {x + r:.1f},{top:.1f} "
            f"H{x + w - r:.1f} Q{x + w:.1f},{top:.1f} {x + w:.1f},{top + r:.1f} V{y0:.1f} Z")


def legend(items, x, y):
    out = []
    for cls, label in items:
        out.append(f'<rect x="{x}" y="{y - 9}" width="10" height="10" rx="2" class="{cls}"/>'
                   f'<text x="{x + 15}" y="{y}" class="legend">{escape(label)}</text>')
        x += 25 + 7 * len(label)
    return "".join(out)


def grouped_bars(path, title, subtitle, categories, series, unit):
    """series: list of (css_class, label, [values per category])."""
    W, H = 760, 360
    left, right, top, bottom = 56, 16, 74, 62
    pw, ph = W - left - right, H - top - bottom
    vmax = nice_max(max(v for _, _, vals in series for v in vals if v is not None))
    y = lambda v: top + ph - v / vmax * ph
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
             f'role="img" aria-label="{escape(title)}">', STYLE,
             f'<rect class="bg" width="{W}" height="{H}" rx="8"/>',
             f'<text x="{left}" y="26" class="title">{escape(title)}</text>',
             f'<text x="{left}" y="44" class="sub">{escape(subtitle)}</text>',
             legend([(c, l) for c, l, _ in series], left, 64)]
    for i in range(5):
        v = vmax * i / 4
        parts.append(f'<line x1="{left}" x2="{W - right}" y1="{y(v):.1f}" y2="{y(v):.1f}" '
                     f'class="{"axis" if i == 0 else "grid"}"/>'
                     f'<text x="{left - 8}" y="{y(v) + 4:.1f}" text-anchor="end" class="tick">{fmt(v)}</text>')
    group_w = pw / len(categories)
    bar_w = min(34, (group_w - 24) / len(series))
    for ci, cat in enumerate(categories):
        gx = left + ci * group_w + (group_w - bar_w * len(series) - 2 * (len(series) - 1)) / 2
        for si, (cls, label, vals) in enumerate(series):
            v = vals[ci]
            if v is None:
                continue
            x = gx + si * (bar_w + 2)
            parts.append(f'<path d="{bar_path(x, y(0), bar_w, max(v / vmax * ph, 1.5))}" class="{cls}">'
                         f'<title>{escape(label)} · {escape(cat)}: {fmt(v)} {unit}</title></path>'
                         f'<text x="{x + bar_w / 2:.1f}" y="{y(v) - 5:.1f}" text-anchor="middle" '
                         f'class="value">{fmt(v)}</text>')
        parts.append(f'<text x="{left + (ci + 0.5) * group_w:.1f}" y="{H - bottom + 18}" text-anchor="middle" '
                     f'class="tick">{escape(cat)}</text>')
    parts.append(f'<text x="{left}" y="{H - 14}" class="sub">{escape(unit)} · median of trials</text></svg>')
    Path(path).write_text("".join(parts))


def window_chart(path, title, subtitle, windows, udp_vals, tcp_val):
    W, H = 760, 340
    left, right, top, bottom = 56, 24, 74, 52
    pw, ph = W - left - right, H - top - bottom
    vmax = nice_max(max(udp_vals + [tcp_val or 0]))
    xs = [left + (i + 0.5) * pw / len(windows) for i in range(len(windows))]
    y = lambda v: top + ph - v / vmax * ph
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
             f'role="img" aria-label="{escape(title)}">', STYLE,
             f'<rect class="bg" width="{W}" height="{H}" rx="8"/>',
             f'<text x="{left}" y="26" class="title">{escape(title)}</text>',
             f'<text x="{left}" y="44" class="sub">{escape(subtitle)}</text>',
             legend([("udp", "UDP Selective Repeat"), ("tcp", "TCP (same link)")], left, 64)]
    for i in range(5):
        v = vmax * i / 4
        parts.append(f'<line x1="{left}" x2="{W - right}" y1="{y(v):.1f}" y2="{y(v):.1f}" '
                     f'class="{"axis" if i == 0 else "grid"}"/>'
                     f'<text x="{left - 8}" y="{y(v) + 4:.1f}" text-anchor="end" class="tick">{fmt(v)}</text>')
    if tcp_val is not None:
        parts.append(f'<line x1="{left}" x2="{W - right}" y1="{y(tcp_val):.1f}" y2="{y(tcp_val):.1f}" '
                     f'class="tcp-line" stroke-width="2" stroke-dasharray="6 4"><title>TCP: {fmt(tcp_val)} Mbit/s'
                     f'</title></line><text x="{W - right}" y="{y(tcp_val) - 6:.1f}" text-anchor="end" '
                     f'class="value">TCP {fmt(tcp_val)}</text>')
    pts = " ".join(f"{x:.1f},{y(v):.1f}" for x, v in zip(xs, udp_vals))
    parts.append(f'<polyline points="{pts}" fill="none" class="udp-line" stroke-width="2"/>')
    for x, w, v in zip(xs, windows, udp_vals):
        parts.append(f'<circle cx="{x:.1f}" cy="{y(v):.1f}" r="5" class="udp ring"><title>window {w}: '
                     f'{fmt(v)} Mbit/s</title></circle>'
                     f'<text x="{x:.1f}" y="{y(v) - 10:.1f}" text-anchor="middle" class="value">{fmt(v)}</text>'
                     f'<text x="{x:.1f}" y="{H - bottom + 18}" text-anchor="middle" class="tick">{w}</text>')
    parts.append(f'<text x="{left}" y="{H - 10}" class="sub">window size (packets; 1 = stop-and-wait) · '
                 f'Mbit/s, median of trials</text></svg>')
    Path(path).write_text("".join(parts))


def human(n):
    for unit, m in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= m:
            return f"{n / m:g} {unit}"
    return f"{n} B"


def main():
    csv_path = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent / "results" / "results.csv")
    out_dir = csv_path.parent
    rows = list(csv.DictReader(open(csv_path)))
    good = [r for r in rows if r["ok"] == "True" and r["verified"] == "True"]
    failed = len(rows) - len(good)

    groups = defaultdict(list)
    for r in good:
        groups[(r["experiment"], r["condition"], int(r["size_bytes"]), r["protocol"], r["window"])].append(r)

    def med(key, field):
        vals = [float(r[field]) for r in groups.get(key, [])]
        return statistics.median(vals) if vals else None

    md = ["# Benchmark results", "",
          "Generated by `bench/benchmark.py`. Each cell is the median of the trials. Network conditions are "
          "applied with netem on the loopback of a private network namespace (MTU 1500, offloads off), "
          "in both directions, identically for UDP and TCP.", ""]
    if failed:
        md += [f"> {failed} run(s) failed or did not verify and are excluded.", ""]

    proto_rows = [r for r in good if r["experiment"] == "protocols"]
    conditions = list(dict.fromkeys(r["condition"] for r in proto_rows))
    sizes = sorted({int(r["size_bytes"]) for r in proto_rows})
    netem = {r["condition"]: r["netem"] for r in rows}
    windows = {r["window"] for r in proto_rows if r["protocol"] == "udp"}
    uw = next(iter(windows)) if windows else ""

    if proto_rows:
        md += ["## UDP (Selective Repeat) vs TCP", "",
               f"UDP window = {uw} packets × 1400 B.", "",
               "| condition | netem | size | UDP time (s) | TCP time (s) | UDP Mbit/s | TCP Mbit/s | "
               "UDP retx | TCP retx | UDP wire overhead | TCP wire overhead |",
               "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for c in conditions:
            for s in sizes:
                u = ("protocols", c, s, "udp", uw)
                t = ("protocols", c, s, "tcp", "")
                cell = lambda k, f, p=3: "–" if med(k, f) is None else f"{med(k, f):.{p}f}"
                md.append(f"| {LABELS.get(c, c)} | `{netem.get(c) or '-'}` | {human(s)} | {cell(u, 'elapsed_s')} | "
                          f"{cell(t, 'elapsed_s')} | {cell(u, 'throughput_mbps', 1)} | "
                          f"{cell(t, 'throughput_mbps', 1)} | {cell(u, 'retransmissions', 0)} | "
                          f"{cell(t, 'retransmissions', 0)} | {cell(u, 'wire_overhead_pct', 1)}% | "
                          f"{cell(t, 'wire_overhead_pct', 1)}% |")
        md.append("")

        big = sizes[-1]
        # Unshaped loopback runs at memory speed and would flatten every other
        # bar, so the charts show only the emulated links (it stays in the table).
        shaped = [c for c in conditions if netem.get(c)] or conditions
        cats = [LABELS.get(c, c) for c in shaped]

        def series(field):
            return [("udp", "UDP Selective Repeat", [med(("protocols", c, big, "udp", uw), field) for c in shaped]),
                    ("tcp", "TCP", [med(("protocols", c, big, "tcp", ""), field) for c in shaped])]

        grouped_bars(out_dir / "throughput.svg", f"Throughput, {human(big)} file",
                     "Higher is better. Same emulated link (20 ms RTT) for both protocols.", cats, series("throughput_mbps"),
                     "Mbit/s")
        grouped_bars(out_dir / "retransmissions.svg", f"Retransmitted packets, {human(big)} file",
                     "UDP: DATA packets re-sent after timeout. TCP: tcpi_total_retrans.", cats,
                     series("retransmissions"), "packets")
        grouped_bars(out_dir / "overhead.svg", f"Protocol overhead on the wire, {human(big)} file",
                     "Extra bytes on the link beyond the file itself: headers, ACKs, retransmissions, duplicates.",
                     cats, series("wire_overhead_pct"), "% of file size")
        md += ["![throughput](throughput.svg)", "", "![retransmissions](retransmissions.svg)", "",
               "![overhead](overhead.svg)", ""]

    sweep = [r for r in good if r["experiment"] == "window_sweep"]
    if sweep:
        cond = sweep[0]["condition"]
        size = int(sweep[0]["size_bytes"])
        ws = sorted({int(r["window"]) for r in sweep if r["protocol"] == "udp"})
        uvals = [med(("window_sweep", cond, size, "udp", str(w)), "throughput_mbps") for w in ws]
        tval = med(("window_sweep", cond, size, "tcp", ""), "throughput_mbps")
        md += [f"## Window size sweep ({LABELS.get(cond, cond)}: `{netem.get(cond)}`, {human(size)})", "",
               "| window | Mbit/s | time (s) | retx |", "|---:|---:|---:|---:|"]
        for w, v in zip(ws, uvals):
            k = ("window_sweep", cond, size, "udp", str(w))
            md.append(f"| {w} | {v:.1f} | {med(k, 'elapsed_s'):.2f} | {med(k, 'retransmissions'):.0f} |")
        if tval is not None:
            md.append(f"| TCP | {tval:.1f} | {med(('window_sweep', cond, size, 'tcp', ''), 'elapsed_s'):.2f} | "
                      f"{med(('window_sweep', cond, size, 'tcp', ''), 'retransmissions'):.0f} |")
        md.append("")
        window_chart(out_dir / "window_sweep.svg", "Throughput vs. sliding-window size",
                     f"{human(size)} file, {netem.get(cond)} (window 1 = stop-and-wait)", ws, uvals, tval)
        md += ["![window sweep](window_sweep.svg)", ""]

    (out_dir / "results.md").write_text("\n".join(md))
    print(f"wrote {out_dir / 'results.md'} and charts")


if __name__ == "__main__":
    main()
