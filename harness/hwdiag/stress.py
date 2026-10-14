"""hwdiag stress: membw on every core while recording frequency, temperature,
utilisation and bandwidth each second; flags thermal throttling."""

from __future__ import annotations

import json
import os
import re
import socket
import statistics
import subprocess
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from . import plots, results, tools


def _under(root: str | Path, path: str) -> Path:
    return Path(root) / path.lstrip("/")


def _read(path: Path) -> str | None:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def _num_key(path: Path) -> list[Any]:
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", path.name)]


def read_freqs(root: str | Path = "/") -> list[float]:
    """Current frequency of every CPU in MHz (scaling_cur_freq is in kHz)."""
    out = []
    for cpu in sorted(_under(root, "/sys/devices/system/cpu").glob("cpu[0-9]*"), key=_num_key):
        v = _read(cpu / "cpufreq" / "scaling_cur_freq")
        if v and v.isdigit():
            out.append(int(v) / 1000.0)
    return out


def read_temps(root: str | Path = "/") -> dict[str, float]:
    """Every hwmon temperature in degrees C, keyed 'chip/label'."""
    temps = {}
    for hw in sorted(_under(root, "/sys/class/hwmon").glob("hwmon*"), key=_num_key):
        chip = _read(hw / "name") or hw.name
        for f in sorted(hw.glob("temp*_input"), key=_num_key):
            v = _read(f)
            if v is None:
                continue
            try:
                value = int(v) / 1000.0  # millidegrees
            except ValueError:
                continue
            stem = f.name[: -len("_input")]
            label = _read(hw / f"{stem}_label") or stem
            temps[f"{chip}/{label}"] = value
    return temps


def read_cpu_times(root: str | Path = "/") -> tuple[int, int] | None:
    """(busy, total) jiffies from the aggregate 'cpu' line of /proc/stat."""
    text = _read(_under(root, "/proc/stat"))
    if not text:
        return None
    for line in text.splitlines():
        if line.startswith("cpu "):
            fields = [int(x) for x in line.split()[1:]]
            idle = fields[3] + (fields[4] if len(fields) > 4 else 0)  # idle + iowait
            total = sum(fields[:8])  # guest time is already inside user/nice
            return total - idle, total
    return None


def cpu_util(prev: tuple[int, int] | None, cur: tuple[int, int] | None) -> float | None:
    if not prev or not cur or cur[1] <= prev[1]:
        return None
    return 100.0 * (cur[0] - prev[0]) / (cur[1] - prev[1])


def read_throttle_count(root: str | Path = "/") -> int | None:
    """Sum of Intel's per-CPU core and package thermal throttle counters."""
    total = None
    for cpu in _under(root, "/sys/devices/system/cpu").glob("cpu[0-9]*"):
        for name in ("core_throttle_count", "package_throttle_count"):
            v = _read(cpu / "thermal_throttle" / name)
            if v and v.isdigit():
                total = (total or 0) + int(v)
    return total


@dataclass
class Throttling:
    detected: bool
    reasons: list[str]
    baseline_mhz: float | None
    intervals: list[tuple[float, float]]


def rolling_median(values: list[float | None], window: int) -> list[float | None]:
    out: list[float | None] = []
    for i in range(len(values)):
        chunk = [v for v in values[max(0, i - window + 1) : i + 1] if v is not None]
        out.append(statistics.median(chunk) if len(chunk) == window else None)
    return out


def detect_throttling(
    samples: list[dict[str, Any]],
    baseline_s: float = 30.0,
    drop_pct: float = 10.0,
    min_samples: int = 5,
    util_min: float = 90.0,
    window: int = 5,
) -> Throttling:
    """Thermal throttling means the frequency fell while the load stayed the
    same: the rolling median frequency stays more than drop_pct below the
    baseline (median of the first baseline_s) for min_samples samples in a
    row while utilisation stays at util_min or above. Rising kernel thermal
    throttle counters also count."""
    reasons: list[str] = []
    intervals: list[tuple[float, float]] = []
    freqs = [s["freq_mhz_mean"] for s in samples]
    early = [s["freq_mhz_mean"] for s in samples if s["t"] <= baseline_s and s["freq_mhz_mean"] is not None]
    baseline = statistics.median(early) if early else None
    if baseline:
        limit = baseline * (1 - drop_pct / 100.0)
        rolled = rolling_median(freqs, window)
        run_start = None
        count = 0
        for i, s in enumerate(samples):
            low = rolled[i] is not None and rolled[i] < limit
            loaded = s["util_pct"] is not None and s["util_pct"] >= util_min
            if low and loaded:
                if run_start is None:
                    run_start = i
                count += 1
            else:
                if run_start is not None and count >= min_samples:
                    intervals.append((samples[run_start]["t"], samples[i - 1]["t"]))
                run_start, count = None, 0
        if run_start is not None and count >= min_samples:
            intervals.append((samples[run_start]["t"], samples[-1]["t"]))
        if intervals:
            low_run = min(r for r in rolled if r is not None)
            reasons.append(
                f"frequency fell to {low_run:.0f} MHz, more than {drop_pct:g}% below the {baseline:.0f} MHz "
                f"baseline, for {len(intervals)} stretch(es) of {min_samples}+ s at >= {util_min:g}% CPU"
            )
    counts = [s["throttle_count"] for s in samples if s["throttle_count"] is not None]
    if len(counts) >= 2 and counts[-1] > counts[0]:
        reasons.append(f"kernel thermal throttle counters rose by {counts[-1] - counts[0]}")
    return Throttling(bool(reasons), reasons, baseline, intervals)


class MembwReader(threading.Thread):
    """Reads membw's one-JSON-line-per-interval output in the background."""

    def __init__(self, proc: subprocess.Popen):
        super().__init__(daemon=True)
        self.proc = proc
        self.latest: float | None = None
        self.lines = 0

    def run(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            try:
                self.latest = float(json.loads(line)["gbs"])
                self.lines += 1
            except (ValueError, KeyError, TypeError):
                continue


def sample_once(root: str | Path, t: float, prev_times, gbs: float | None) -> tuple[dict[str, Any], Any]:
    freqs = read_freqs(root)
    temps = read_temps(root)
    times = read_cpu_times(root)
    sample = {
        "t": round(t, 3),
        "freq_mhz_mean": statistics.fmean(freqs) if freqs else None,
        "freq_mhz_min": min(freqs) if freqs else None,
        "freq_mhz_max": max(freqs) if freqs else None,
        "temp_c_max": max(temps.values()) if temps else None,
        "temps_c": temps,
        "util_pct": cpu_util(prev_times, times),
        "gbs": gbs,
        "throttle_count": read_throttle_count(root),
    }
    return sample, times


def default_out_dir() -> Path:
    host = re.sub(r"[^A-Za-z0-9._-]", "_", socket.gethostname()) or "host"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return tools.repo_root() / "results" / f"stress-{host}-{stamp}"


def command(args) -> int:
    seconds = args.seconds if args.seconds is not None else args.minutes * 60.0
    if seconds <= 0 or args.interval <= 0:
        print("hwdiag: --minutes/--seconds and --interval must be positive")
        return 1
    out_dir = Path(args.out) if args.out else default_out_dir()
    out_dir.mkdir(parents=True, exist_ok=True)
    try:
        exe = tools.find_tool("membw", args.bin_dir)
    except tools.ToolNotFound as e:
        print(f"hwdiag: {e}")
        return 1
    membw_out = out_dir / "membw_stress.json"
    commit = tools.git_commit()
    env = {"HWDIAG_GIT_COMMIT": commit} if commit else {}
    cmd = [
        str(exe),
        "--duration",
        str(seconds),
        "--interval",
        str(args.interval),
        "--quiet",
        "--out",
        str(membw_out),
    ]
    print(f"hwdiag: stressing memory on all cores for {seconds:g} s; sampling every {args.interval:g} s")

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, env={**os.environ, **env})
    reader = MembwReader(proc)
    reader.start()
    samples: list[dict[str, Any]] = []
    t0 = time.monotonic()
    _, prev_times = sample_once(args.root, 0.0, None, None)
    next_t = t0 + args.interval
    while proc.poll() is None:
        time.sleep(max(0.0, next_t - time.monotonic()))
        next_t += args.interval
        if proc.poll() is not None:
            break
        s, prev_times = sample_once(args.root, time.monotonic() - t0, prev_times, reader.latest)
        samples.append(s)
        if not args.quiet and len(samples) % max(1, int(10 / args.interval)) == 0:
            f = f"{s['freq_mhz_mean']:.0f} MHz" if s["freq_mhz_mean"] else "freq n/a"
            temp = f"{s['temp_c_max']:.1f} C" if s["temp_c_max"] is not None else "temp n/a"
            gbs = f"{s['gbs']:.1f} GB/s" if s["gbs"] is not None else "GB/s n/a"
            print(f"hwdiag: t={s['t']:.0f}s {f}, {temp}, {gbs}", flush=True)
    reader.join(timeout=5)
    if proc.returncode != 0:
        print(f"hwdiag: membw exited with code {proc.returncode}")
        return 1

    notes = []
    if all(s["freq_mhz_mean"] is None for s in samples):
        notes.append("cpufreq not available: frequency-based throttling detection skipped (normal in VMs)")
    if all(s["temp_c_max"] is None for s in samples):
        notes.append("no hwmon temperature sensors (normal in VMs; on hardware try: sudo sensors-detect)")
    if all(s["throttle_count"] is None for s in samples):
        notes.append("no thermal_throttle counters (Intel-only sysfs files)")
    th = detect_throttling(samples)
    host = None
    warnings = []
    if membw_out.exists():
        membw_doc = results.load(membw_out)
        host = membw_doc["host"]
        warnings = membw_doc["warnings"]

    def stat(key, fn):
        vals = [s[key] for s in samples if s[key] is not None]
        return fn(vals) if vals else None

    doc = results.make_result(
        "stress",
        {"seconds": seconds, "interval_s": args.interval, "root": str(args.root), "membw": cmd[:-1]},
        {
            "samples": samples,
            "throttling": {
                "detected": th.detected,
                "reasons": th.reasons,
                "baseline_mhz": th.baseline_mhz,
                "intervals": [list(i) for i in th.intervals],
            },
            "summary": {
                "samples": len(samples),
                "mean_gbs": stat("gbs", statistics.fmean),
                "freq_mhz_min": stat("freq_mhz_min", min),
                "freq_mhz_max": stat("freq_mhz_max", max),
                "temp_c_max": stat("temp_c_max", max),
                "util_pct_mean": stat("util_pct", statistics.fmean),
            },
            "notes": notes,
        },
        warnings=warnings,
        host=host,
        git_commit=commit or "unknown",
    )
    results.save(doc, out_dir / "stress.json")
    plots.stress(samples, th.intervals, out_dir / "stress.png")
    print(f"hwdiag: wrote {out_dir / 'stress.json'} and {out_dir / 'stress.png'}")
    for n in notes:
        print(f"  note: {n}")
    if th.detected:
        for r in th.reasons:
            print(f"  THROTTLING: {r}")
        return 2
    print("  no thermal throttling detected")
    return 0
