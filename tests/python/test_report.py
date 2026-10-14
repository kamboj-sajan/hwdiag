from __future__ import annotations

import argparse
import json
import shutil

import pytest
from conftest import SAMPLE_RUN
from hwdiag import report, results

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def png_ok(path):
    return path.exists() and path.stat().st_size > 5000 and path.read_bytes()[:8] == PNG_MAGIC


def test_report_from_the_sample_run(tmp_path):
    out = tmp_path / "report"
    path = report.build(SAMPLE_RUN, out)
    text = path.read_text()
    for heading in (
        "## Summary",
        "## Inventory",
        "## Memory latency (memlat)",
        "## Memory bandwidth (membw)",
        "## Hardware counters (perfctr)",
        "## Memory test (memtest)",
        "## Warnings",
    ):
        assert heading in text
    for png in ("latency.png", "latency_modes.png", "bandwidth.png"):
        assert png_ok(out / png), png
        assert f"]({png})" in text
    assert "No inventory warnings." in text
    assert "Hardware counters were unavailable" in text  # the sample comes from a VM
    assert "Not compared:" in text
    assert "| DRAM latency (random, 4 KiB pages) |" in text


def test_report_command_writes_into_the_run_dir_by_default(tmp_path, capsys):
    run = tmp_path / "run"
    shutil.copytree(SAMPLE_RUN, run)
    assert report.command(argparse.Namespace(run=str(run / "run.json"), out=None)) == 0
    assert (run / "REPORT.md").exists()
    assert png_ok(run / "latency.png")


def fake_reading(cycles, instructions, l1d, dtlb, loads):
    ev = lambda v: {"raw": int(v), "scaled": v, "time_enabled_ns": 10, "time_running_ns": 5, "scale": 2.0}  # noqa: E731
    return {
        "available": True,
        "reason": None,
        "groups": 2,
        "multiplexed": True,
        "max_scale": 2.0,
        "events": {"cycles": ev(cycles), "instructions": ev(instructions)},
        "derived": {"ipc": instructions / cycles, "cache_misses_pki": 1.5},
        "per_load": {"cycles": cycles / loads, "l1d_read_misses": l1d, "dtlb_read_misses": dtlb},
        "dropped": [],
    }


def test_report_shows_counter_tables_and_perf_stat_check_when_available(tmp_path):
    run = tmp_path / "run"
    shutil.copytree(SAMPLE_RUN, run)
    lat = json.loads((run / "memlat.json").read_text())
    lat["results"]["counters"] = {
        "available": True,
        "reason": None,
        "group_layout": [["cycles", "instructions"]],
    }
    for curve in lat["results"]["curves"]:
        for p in curve["points"]:
            p["counters"] = fake_reading(300.0, 100.0, 1.0, 0.5, 1.0)
    results.save(lat, run / "memlat.json")
    pc = json.loads((run / "perfcheck.json").read_text())
    pc["results"]["perf_stat"] = {
        "available": True,
        "reason": None,
        "events": {
            "cycles": {"perf_stat": 1000.0, "perfctr": 990.0, "diff_pct": -1.0},
            "instructions": {"perf_stat": 2000.0, "perfctr": 1999.0, "diff_pct": -0.05},
        },
        "tolerance_pct": 5.0,
        "within_tolerance": True,
    }
    results.save(pc, run / "perfcheck.json")
    text = report.build(run).read_text()
    assert "| size | IPC | cycles | L1D misses | dTLB misses |" in text
    assert "| 4K | 0.333 | 300.0 | 1.000 | 0.500 | 1.5 | yes |" in text
    assert "within the 5% tolerance" in text
    assert "-1.00%" in text


def test_report_lists_memtest_errors(tmp_path):
    run = tmp_path / "run"
    shutil.copytree(SAMPLE_RUN, run)
    mt = json.loads((run / "memtest.json").read_text())
    mt["results"]["errors"] = [
        {
            "pattern": "zeros",
            "offset": 4096,
            "offset_hex": "0x0000000000001000",
            "expected": "0x0000000000000000",
            "actual": "0x0000000000000020",
            "xor_mask": "0x0000000000000020",
            "flipped_bits": [5],
            "physical_address": None,
        }
    ]
    mt["results"]["total_errors"] = 1
    mt["results"]["status"] = "fail"
    results.save(mt, run / "memtest.json")
    text = report.build(run).read_text()
    assert "First errors:" in text
    assert "| zeros | 0x0000000000001000 |" in text


def test_numa_section_appears_with_two_nodes(tmp_path):
    run = tmp_path / "run"
    shutil.copytree(SAMPLE_RUN, run)
    (run / "numa").mkdir()
    base = json.loads((SAMPLE_RUN / "memlat.json").read_text())
    for node, factor in ((0, 1.0), (1, 1.6)):
        doc = json.loads(json.dumps(base))
        doc["params"]["mem_node"] = node
        doc["results"]["curves"] = doc["results"]["curves"][:1]
        for p in doc["results"]["curves"][0]["points"]:
            p["median_ns"] *= factor
        results.save(doc, run / "numa" / f"memlat_node{node}.json")
    text = report.build(run).read_text()
    assert "## NUMA: local vs remote latency" in text
    assert png_ok(run / "numa.png")


def test_report_needs_results(tmp_path):
    with pytest.raises(results.ResultError):
        report.build(tmp_path)
