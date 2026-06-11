#!/usr/bin/env python3
"""
bench.py – Automated latency benchmark at 250 k, 1 M, and 3.5 M orders/s.

For each target level, 5 valid 15-second samples are collected and their
HDR-histogram percentiles are averaged.  A sample is accepted only when the
measured requests/s falls within the declared tolerance band.  Samples outside
the band are silently discarded; up to MAX_ATTEMPTS runs are made per level.

Usage:
    python3 bench.py [--clients N]   (default: 10)

Logs for every run are saved to bench_logs/ for post-hoc inspection.
EXPECTED_THROUGHPUT is restored to its original value on exit.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

# paths

ROOT         = Path(__file__).resolve().parent
BUILD_DIR    = ROOT / "cmake-build-debug"
EXCHANGE_BIN = BUILD_DIR / "exchange" / "exchange-release"
CLIENT_BIN   = BUILD_DIR / "client"   / "client-release"
CLIENT_CFG   = ROOT / "client" / "config" / "config.h"
LOG_DIR      = ROOT / "bench_logs"

# benchmark parameters

RUN_DURATION   = 15    # seconds per sample
SAMPLES_NEEDED = 5
MAX_ATTEMPTS   = 25    # discard guard: bail out if we can't get 5 valid samples

LEVELS = [
    dict(name="250k",  target=250_000,   tolerance=50_000),
    dict(name="1M",    target=1_000_000, tolerance=100_000),
    dict(name="3.5M",  target=3_500_000, tolerance=250_000),
]

# Percentile labels as emitted by print_hist() in latency.h
PCTS = ["p50", "p90", "p99", "p99.9", "p99.99", "max"]

# Desired display order; any extra sections found are appended after.
SECTION_ORDER = [
    "begin read -> end write   (end-to-end)",
    "begin read -> end read    (recv syscall)",
    "end read -> push inbound  (inbound thread)",
    "push inbound -> engine pop (ring transit)",
    "engine pop -> engine push (engine processing)",
    "engine push -> server pop (ring transit)",
    "pop outbound -> begin write (outbound thread)",
    "begin write -> end write  (send syscall)",
]

# config manipulation

_THROUGHPUT_RE = re.compile(r"(#define EXPECTED_THROUGHPUT)\s+([\d'_]+)")


def read_throughput_token() -> str:
    """Return the raw token currently in config.h (e.g. "500'000")."""
    m = _THROUGHPUT_RE.search(CLIENT_CFG.read_text())
    return m.group(2) if m else "0"


def _write_throughput(token: str) -> None:
    text = CLIENT_CFG.read_text()
    text = _THROUGHPUT_RE.sub(rf"\1 {token}", text)
    CLIENT_CFG.write_text(text)


def set_throughput(value: int) -> None:
    _write_throughput(str(value))


def restore_throughput(original_token: str) -> None:
    _write_throughput(original_token)

# build

def rebuild_client() -> None:
    """Rebuild only client-release; exchange config is untouched."""
    r = subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "--target", "client-release", "-j4"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if r.returncode != 0:
        sys.exit(f"[ERROR] Build failed:\n{r.stderr.decode()}")

# single run 

def run_sample(num_clients: int, log_path: Path) -> tuple[str, str]:
    """
    Start exchange, then client; let them run for RUN_DURATION seconds.
    Stops client first (no more sends), then exchange (flushes LatencyHandler).
    Returns (client_stderr, exchange_stdout).
    """
    xproc = subprocess.Popen(
        [str(EXCHANGE_BIN)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    time.sleep(0.5)  # let the exchange bind its socket

    if xproc.poll() is not None:
        raise RuntimeError("exchange exited immediately – port already in use?")

    cproc = subprocess.Popen(
        [str(CLIENT_BIN), str(num_clients)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )

    time.sleep(RUN_DURATION)

    # Stop client first so the exchange drains cleanly.
    cproc.send_signal(signal.SIGTERM)
    try:
        _, c_err = cproc.communicate(timeout=8)
    except subprocess.TimeoutExpired:
        cproc.kill()
        _, c_err = cproc.communicate()

    # Stopping the exchange triggers the LatencyHandler destructor → histogram output.
    xproc.send_signal(signal.SIGTERM)
    try:
        x_out, x_err = xproc.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        xproc.kill()
        x_out, x_err = xproc.communicate()

    c_err_s = c_err.decode(errors="replace")
    x_out_s = x_out.decode(errors="replace")

    log_path.write_text(
        "=== CLIENT STDERR ===\n" + c_err_s
        + "\n=== EXCHANGE STDOUT ===\n" + x_out_s
        + "\n=== EXCHANGE STDERR ===\n" + x_err.decode(errors="replace"),
    )

    time.sleep(1)  # let the port leave TIME_WAIT before the next run
    return c_err_s, x_out_s

# parsers 

def parse_throughput(client_stderr: str) -> float | None:
    m = re.search(r"requests/s\s*:\s*([\d.]+)", client_stderr)
    return float(m.group(1)) if m else None


def parse_histograms(exchange_stdout: str) -> dict[str, dict[str, int]]:
    """
    Parse all print_hist() blocks from exchange stdout.
    Section titles are identified by containing ' -> '.
    Data lines match:  p<digits[.digits]>   <int> ns
    """
    result: dict[str, dict[str, int]] = {}
    current: str | None = None

    for raw in exchange_stdout.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = re.match(r"^(p[\d.]+|max)\s+(\d+)\s+ns$", line)
        if m and current is not None:
            result.setdefault(current, {})[m.group(1)] = int(m.group(2))
        elif " -> " in line:
            current = line

    return result

#  statistics 

def average_samples(
    samples: list[dict[str, dict[str, int]]],
) -> dict[str, dict[str, float]]:
    """Average each percentile across all samples per section."""
    all_secs = {sec for s in samples for sec in s}
    averaged: dict[str, dict[str, float]] = {}
    for sec in all_secs:
        averaged[sec] = {}
        for pct in PCTS:
            vals = [s[sec][pct] for s in samples if sec in s and pct in s[sec]]
            if vals:
                averaged[sec][pct] = sum(vals) / len(vals)
    return averaged

# reporting

_COL  = 50            # section label width
_PCOL = 9             # per-percentile column width
_BAR  = "=" * (_COL + 2 + _PCOL * len(PCTS))

def _pct_header() -> str:
    return "  " + f"{'Section':<{_COL}}" + "".join(f"{p:>{_PCOL}}" for p in PCTS)

def _pct_row(label: str, d: dict[str, float]) -> str:
    label = label if len(label) <= _COL else label[:_COL - 3] + "..."
    vals  = "".join(f"{d.get(p, 0):>{_PCOL}.0f}" for p in PCTS)
    return f"  {label:<{_COL}}{vals}"

def print_results(
    name: str,
    rates: list[float],
    averaged: dict[str, dict[str, float]],
) -> None:
    mean = sum(rates) / len(rates)
    print(f"\n{_BAR}")
    print(f"  Level : {name}")
    print(f"  Samples : {len(rates)}   "
          f"mean {mean:>10,.0f} req/s   "
          f"[min {min(rates):,.0f}  max {max(rates):,.0f}]")
    print(_BAR)
    print(_pct_header())
    print("  " + "-" * (_COL + _PCOL * len(PCTS)))

    ordered = [s for s in SECTION_ORDER if s in averaged]
    extras  = [s for s in averaged    if s not in SECTION_ORDER]

    for sec in ordered + extras:
        print(_pct_row(sec, averaged[sec]))
    print()

# main 

def preflight() -> None:
    missing = [p for p in (EXCHANGE_BIN, CLIENT_BIN) if not p.exists()]
    if missing:
        sys.exit(
            "[ERROR] Binaries not found:\n"
            + "\n".join(f"  {p}" for p in missing)
            + f"\nBuild the project first:  cmake --build {BUILD_DIR}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clients", type=int, default=10,
                        help="concurrent TCP connections (default: 10)")
    args = parser.parse_args()

    preflight()
    LOG_DIR.mkdir(exist_ok=True)
    original_token = read_throughput_token()

    try:
        for level in LEVELS:
            name      = level["name"]
            target    = level["target"]
            tolerance = level["tolerance"]
            lo, hi    = target - tolerance, target + tolerance

            print(f"\n{'─' * 80}")
            print(f"  Level {name}  |  target {target:,} ± {tolerance:,} req/s"
                  f"  (accept band: {lo:,} – {hi:,})")
            print(f"{'─' * 80}")

            print(f"  Setting EXPECTED_THROUGHPUT = {target} and rebuilding …", flush=True)
            set_throughput(target)
            rebuild_client()
            print("  Build OK.\n")

            valid_rates: list[float]                     = []
            valid_hists: list[dict[str, dict[str, int]]] = []
            attempts = 0

            while len(valid_rates) < SAMPLES_NEEDED and attempts < MAX_ATTEMPTS:
                attempts += 1
                log = LOG_DIR / f"{name}_run_{attempts:02d}.log"
                print(f"  Run {attempts:2d}  ", end="", flush=True)

                try:
                    c_err, x_out = run_sample(args.clients, log)
                except RuntimeError as exc:
                    print(f"[SKIP – {exc}]")
                    time.sleep(2)
                    continue

                rate  = parse_throughput(c_err)
                hists = parse_histograms(x_out)

                if rate is None:
                    print(f"[SKIP – could not parse throughput]  → {log.name}")
                    continue
                if not hists:
                    print(f"rate={rate:>10,.0f}  [SKIP – no latency histograms]  → {log.name}")
                    continue

                in_band = lo <= rate <= hi
                status  = "✓" if in_band else f"✗  (outside [{lo:,}–{hi:,}])"
                progress = f"  ({len(valid_rates)+1}/{SAMPLES_NEEDED})" if in_band else ""
                print(f"rate={rate:>10,.0f}  {status}{progress}")

                if in_band:
                    valid_rates.append(rate)
                    valid_hists.append(hists)

            if not valid_rates:
                print(f"\n  [WARN] 0 valid samples after {attempts} attempts for level {name}.")
                print(f"         Try adjusting EXPECTED_THROUGHPUT manually "
                      f"or increasing MAX_ATTEMPTS.\n")
                continue

            if len(valid_rates) < SAMPLES_NEEDED:
                print(f"\n  [WARN] Only {len(valid_rates)}/{SAMPLES_NEEDED} valid samples collected.\n")

            print_results(name, valid_rates, average_samples(valid_hists))

    finally:
        restore_throughput(original_token)
        print(f"[Restored EXPECTED_THROUGHPUT = {original_token}]\n")


if __name__ == "__main__":
    main()
