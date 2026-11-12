#!/usr/bin/env python3
"""Benchmark the Selective Repeat UDP protocol against TCP.

Every run happens inside a private network namespace (`unshare -rn`, no root
needed) whose loopback interface is shaped with Linux netem. Both protocols
therefore go through exactly the same emulated link: same delay, same loss,
same duplication, same reordering, applied in both directions.

The loopback is configured like a real Ethernet link (MTU 1500, TSO/GSO/GRO
off) so TCP cannot cheat with 64 KB super-segments that netem would drop or
keep as a single unit.

Per run we record:
  time / throughput      end-to-end, until the receiver confirms the file is on disk
  retransmissions        UDP: from the sender; TCP: tcpi_total_retrans (TCP_INFO)
  wire bytes / packets   delta of the namespace's loopback counters (/proc/net/dev):
                         every header, ACK, retransmission and duplicate counts
  overhead               wire bytes / file bytes - 1

Usage:
  python3 bench/benchmark.py                 # full matrix (several minutes)
  python3 bench/benchmark.py --quick         # smaller sizes, 1 trial
"""

import argparse
import csv
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UDP_PORT, TCP_PORT = 9000, 9001

# name -> netem arguments ("" = no shaping). Delay applies per direction, so
# "delay 10ms" gives a 20 ms round-trip time.
CONDITIONS = [
    ("clean",            ""),
    ("rtt20",            "delay 10ms"),
    ("rtt20_loss1",      "delay 10ms loss 1%"),
    ("rtt20_loss5",      "delay 10ms loss 5%"),
    ("rtt20_loss10",     "delay 10ms loss 10%"),
    ("rtt20_dup_reorder", "delay 10ms 2ms loss 2% duplicate 5% reorder 25% 50%"),
]
WINDOW_SWEEP_CONDITION = ("rtt20_loss1", "delay 10ms loss 1%")
WINDOW_SWEEP = [1, 4, 16, 64, 256, 512]


def sh(cmd):
    subprocess.run(cmd, shell=True, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def setup_namespace():
    sh("ip link set lo up mtu 1500")
    sh("ethtool -K lo tso off gso off gro off 2>/dev/null || true")


def set_netem(netem):
    subprocess.run("tc qdisc del dev lo root", shell=True, stderr=subprocess.DEVNULL)
    if netem:
        sh(f"tc qdisc add dev lo root netem limit 10000 {netem}")


def lo_counters():
    for line in Path("/proc/net/dev").read_text().splitlines():
        if line.strip().startswith("lo:"):
            f = line.split(":", 1)[1].split()
            return int(f[8]), int(f[9])  # tx bytes, tx packets
    raise RuntimeError("no lo in /proc/net/dev")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def parse_size(s):
    s = s.strip().upper()
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    return int(float(s[:-1]) * mult[s[-1]]) if s[-1] in mult else int(s)


def human(n):
    for unit, m in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= m:
            return f"{n / m:g}{unit}"
    return f"{n}B"


class Servers:
    def __init__(self, build, out_dir):
        self.out_dir = out_dir
        log = open(out_dir.parent / "servers.log", "a")
        self.procs = [
            subprocess.Popen([build / "udp_server", "--port", str(UDP_PORT), "--out-dir", out_dir],
                             stderr=log, stdout=log),
            subprocess.Popen([build / "tcp_server", "--port", str(TCP_PORT), "--out-dir", out_dir],
                             stderr=log, stdout=log),
        ]
        time.sleep(0.3)

    def close(self):
        for p in self.procs:
            p.send_signal(signal.SIGTERM)
        for p in self.procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


def run_transfer(build, protocol, src, out_dir, window, timeout):
    name = f"{protocol}_{src.name}"
    if protocol == "udp":
        cmd = [build / "udp_client", src, "--port", str(UDP_PORT), "--name", name,
               "--window", str(window), "--json", "--max-retries", "60"]
    else:
        cmd = [build / "tcp_client", src, "--port", str(TCP_PORT), "--name", name, "--json"]

    bytes0, pkts0 = lo_counters()
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "timeout"}
    bytes1, pkts1 = lo_counters()
    if res.returncode != 0:
        return {"ok": False, "error": res.stderr.strip()}

    stats = json.loads(res.stdout)
    received = out_dir / f"received_{name}"
    stats["verified"] = received.exists() and sha256(received) == sha256(src)
    received.unlink(missing_ok=True)
    stats["wire_bytes"] = bytes1 - bytes0
    stats["wire_packets"] = pkts1 - pkts0
    size = stats["file_size"]
    stats["wire_overhead_pct"] = 100.0 * (stats["wire_bytes"] / size - 1) if size else 0.0
    return stats


FIELDS = ["experiment", "condition", "netem", "size_bytes", "protocol", "window", "trial", "ok", "verified",
          "elapsed_s", "throughput_mbps", "retransmissions", "wire_bytes", "wire_packets", "wire_overhead_pct",
          "error"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=str(ROOT / "build"))
    ap.add_argument("--sizes", default="100K,1M,10M")
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--window", type=int, default=64, help="UDP window for the protocol comparison")
    ap.add_argument("--out", default=str(ROOT / "bench" / "results"))
    ap.add_argument("--quick", action="store_true", help="100K,1M with 1 trial")
    ap.add_argument("--no-window-sweep", action="store_true")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    if os.environ.get("RUDP_IN_NETNS") != "1":
        # Re-exec ourselves inside a fresh user+network namespace.
        env = dict(os.environ, RUDP_IN_NETNS="1")
        os.execvpe("unshare", ["unshare", "-rn", sys.executable, *sys.argv], env)

    if args.quick:
        args.sizes, args.trials = "100K,1M", 1
    build = Path(args.build).resolve()
    for exe in ("udp_client", "udp_server", "tcp_client", "tcp_server"):
        if not (build / exe).exists():
            sys.exit(f"missing {build / exe}; build first: cmake -S . -B build && cmake --build build")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="rudp-bench-"))
    recv_dir = work / "received"
    recv_dir.mkdir()
    sizes = [parse_size(s) for s in args.sizes.split(",")]

    setup_namespace()
    files = {}
    for size in sizes + [10 << 20]:
        if size not in files:
            p = work / f"file_{human(size)}.bin"
            p.write_bytes(os.urandom(size))
            files[size] = p

    rows = []
    csv_path = out / "results.csv"
    fh = open(csv_path, "w", newline="")
    writer = csv.DictWriter(fh, fieldnames=FIELDS, extrasaction="ignore")
    writer.writeheader()

    def record(experiment, cond, netem, size, protocol, window, trial, stats):
        row = {"experiment": experiment, "condition": cond, "netem": netem, "size_bytes": size,
               "protocol": protocol, "window": window if protocol == "udp" else "", "trial": trial,
               "error": stats.get("error", ""), **{k: v for k, v in stats.items() if k in FIELDS}}
        rows.append(row)
        writer.writerow(row)
        fh.flush()
        status = "ok" if stats.get("ok") and stats.get("verified") else f"FAILED {stats.get('error', 'verify')}"
        print(f"  {protocol.upper():3} {human(size):>6} w={window if protocol == 'udp' else '-':<4} "
              f"#{trial}: {stats.get('elapsed_s', 0):8.3f}s {stats.get('throughput_mbps', 0):9.2f} Mbit/s "
              f"retx={stats.get('retransmissions', '-'):<6} overhead={stats.get('wire_overhead_pct', 0):6.1f}% "
              f"{status}", flush=True)

    plan = [("protocols", c, n) for c, n in CONDITIONS]
    if not args.no_window_sweep:
        plan.append(("window_sweep",) + WINDOW_SWEEP_CONDITION)

    try:
        for experiment, cond, netem in plan:
            print(f"\n== {experiment}: {cond} ({netem or 'no shaping'})", flush=True)
            set_netem(netem)
            servers = Servers(build, recv_dir)
            try:
                if experiment == "protocols":
                    for size in sizes:
                        for trial in range(1, args.trials + 1):
                            for proto in ("udp", "tcp"):
                                stats = run_transfer(build, proto, files[size], recv_dir, args.window, args.timeout)
                                record(experiment, cond, netem, size, proto, args.window, trial, stats)
                else:
                    size = 10 << 20
                    for window in WINDOW_SWEEP:
                        for trial in range(1, args.trials + 1):
                            stats = run_transfer(build, "udp", files[size], recv_dir, window, args.timeout)
                            record(experiment, cond, netem, size, "udp", window, trial, stats)
                    for trial in range(1, args.trials + 1):
                        stats = run_transfer(build, "tcp", files[size], recv_dir, 0, args.timeout)
                        record(experiment, cond, netem, size, "tcp", 0, trial, stats)
            finally:
                servers.close()
    finally:
        fh.close()
        set_netem("")
        shutil.rmtree(work, ignore_errors=True)

    print(f"\nwrote {csv_path}")
    subprocess.run([sys.executable, str(ROOT / "bench" / "report.py"), str(csv_path)], check=False)


if __name__ == "__main__":
    main()
