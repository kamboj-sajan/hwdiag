from __future__ import annotations

import argparse
import sys

from conftest import SYS
from hwdiag import results, stress

DESKTOP = SYS / "desktop"
SERVER = SYS / "server"


def series(freqs, util=100.0, start=0, throttle=None):
    out = []
    for i, f in enumerate(freqs):
        out.append(
            {
                "t": float(start + i),
                "freq_mhz_mean": f,
                "util_pct": util if not callable(util) else util(i),
                "throttle_count": None if throttle is None else throttle(i),
            }
        )
    return out


def test_steady_frequency_is_not_throttling():
    th = stress.detect_throttling(series([3000.0] * 120))
    assert not th.detected
    assert th.baseline_mhz == 3000.0
    assert th.intervals == []


def test_sustained_drop_under_constant_load_is_throttling():
    th = stress.detect_throttling(series([3000.0] * 40 + [2500.0] * 20 + [3000.0] * 20))
    assert th.detected
    assert len(th.intervals) == 1
    start, end = th.intervals[0]
    assert 40 <= start <= 45 and 55 <= end <= 64
    assert "below the 3000 MHz baseline" in th.reasons[0]


def test_drop_while_the_load_also_drops_is_not_throttling():
    th = stress.detect_throttling(
        series([3000.0] * 40 + [2500.0] * 20, util=lambda i: 100.0 if i < 40 else 40.0)
    )
    assert not th.detected


def test_small_drop_is_within_the_threshold():
    assert not stress.detect_throttling(series([3000.0] * 40 + [2800.0] * 20)).detected  # 6.7% < 10%


def test_short_dip_is_ignored():
    assert not stress.detect_throttling(series([3000.0] * 40 + [2000.0] * 3 + [3000.0] * 20)).detected


def test_rising_kernel_throttle_counters_count_as_throttling():
    th = stress.detect_throttling(series([3000.0] * 20, throttle=lambda i: 0 if i < 10 else 4))
    assert th.detected
    assert "rose by 4" in th.reasons[0]


def test_no_frequency_data_means_no_frequency_verdict():
    th = stress.detect_throttling(series([None] * 60))
    assert not th.detected
    assert th.baseline_mhz is None


def test_rolling_median_needs_a_full_window():
    assert stress.rolling_median([1.0, 2.0, 3.0, 4.0], 3) == [None, None, 2.0, 3.0]


def test_samplers_read_the_desktop_fixture():
    freqs = stress.read_freqs(DESKTOP)
    assert freqs[0] == 4400.0 and freqs[-1] == 3700.0 and len(freqs) == 8
    temps = stress.read_temps(DESKTOP)
    assert temps == {"coretemp/Package id 0": 91.0, "coretemp/Core 0": 55.0, "nvme/Composite": 42.85}
    assert stress.read_cpu_times(DESKTOP) == (1500, 9600)  # busy = total - idle - iowait
    assert stress.read_throttle_count(DESKTOP) == sum(range(8)) + 2


def test_samplers_on_a_machine_without_sensors():
    assert stress.read_freqs(SERVER) == []
    assert stress.read_throttle_count(SERVER) is None
    assert stress.read_cpu_times(SERVER) is None
    sample, _ = stress.sample_once(SERVER, 1.0, None, None)
    assert sample["freq_mhz_mean"] is None
    assert sample["util_pct"] is None


def test_cpu_util_from_two_snapshots():
    assert stress.cpu_util((100, 1000), (190, 1100)) == 90.0
    assert stress.cpu_util(None, (1, 2)) is None
    assert stress.cpu_util((5, 10), (5, 10)) is None


FAKE_MEMBW = """#!{python}
import json, sys, time
args = sys.argv[1:]
duration = float(args[args.index("--duration") + 1])
interval = float(args[args.index("--interval") + 1])
t = 0.0
while t < duration:
    time.sleep(interval)
    t += interval
    print(json.dumps({{"t": t, "gbs": 20.0}}), flush=True)
"""


def test_stress_command_end_to_end_with_a_fake_membw(tmp_path, monkeypatch):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    exe = bin_dir / "membw"
    exe.write_text(FAKE_MEMBW.format(python=sys.executable))
    exe.chmod(0o755)
    out = tmp_path / "stress"
    args = argparse.Namespace(
        minutes=10.0,
        seconds=0.6,
        interval=0.1,
        root=str(DESKTOP),
        out=str(out),
        bin_dir=str(bin_dir),
        quiet=True,
    )
    # The fixture's thermal_throttle counters never change, so no throttling.
    assert stress.command(args) == 0
    doc = results.load(out / "stress.json")
    r = doc["results"]
    assert r["samples"], "no samples recorded"
    assert r["samples"][-1]["freq_mhz_mean"] == sum(4400 - i * 100 for i in range(8)) / 8
    assert r["summary"]["temp_c_max"] == 91.0
    assert r["throttling"]["detected"] is False
    assert (out / "stress.png").stat().st_size > 5000
    assert any(s["gbs"] == 20.0 for s in r["samples"])


def test_stress_rejects_non_positive_durations(tmp_path):
    args = argparse.Namespace(
        minutes=0.0, seconds=None, interval=1.0, root="/", out=str(tmp_path), bin_dir=None, quiet=True
    )
    assert stress.command(args) == 1
