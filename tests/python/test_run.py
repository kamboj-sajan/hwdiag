from __future__ import annotations

import argparse

from conftest import SAMPLE_RUN, load_json
from hwdiag import results
from hwdiag import run as run_mod


def run_args(out, bin_dir, suite="quick"):
    return argparse.Namespace(suite=suite, out=str(out), bin_dir=str(bin_dir))


def test_quick_suite_runs_every_tool_and_writes_a_valid_manifest(tmp_path, fake_bin, monkeypatch):
    monkeypatch.setattr(run_mod.shutil, "which", lambda name: None)  # no perf
    out = tmp_path / "run"
    assert run_mod.command(run_args(out, fake_bin)) == 0
    names = [c[0] for c in fake_bin.calls()]
    assert names == ["inventory", "memlat", "membw", "memtest", "perfcheck"]
    manifest = results.load(out / "run.json")
    r = manifest["results"]
    assert r["suite"] == "quick"
    assert set(r["files"]) == {"inventory", "memlat", "membw", "memtest", "perfcheck"}
    assert all(t["exit_code"] == 0 for t in r["tools"])
    sample = load_json(SAMPLE_RUN / "memlat.json")
    assert r["summary"]["dram_latency_ns"] == sample["results"]["dram_latency_ns"]
    assert manifest["host"] == load_json(SAMPLE_RUN / "inventory.json")["host"]


def test_suite_arguments_are_passed_to_the_tools(tmp_path, fake_bin, monkeypatch):
    monkeypatch.setattr(run_mod.shutil, "which", lambda name: None)
    run_mod.command(run_args(tmp_path / "run", fake_bin))
    calls = {c[0]: c[1:] for c in fake_bin.calls()}
    assert calls["memlat"][:2] == ["--max", "64M"]
    assert "--modes" in calls["memlat"]
    assert calls["memtest"][:2] == ["--size", "64M"]
    assert all("--out" in args for args in calls.values())


def test_full_suite_uses_bigger_settings(tmp_path, fake_bin, monkeypatch):
    monkeypatch.setattr(run_mod.shutil, "which", lambda name: None)
    assert run_mod.command(run_args(tmp_path / "run", fake_bin, suite="full")) == 0
    calls = {c[0]: c[1:] for c in fake_bin.calls()}
    assert calls["memlat"][:2] == ["--max", "1G"]
    assert calls["memtest"][0] == "--out"  # default size: 25% of MemAvailable


def test_a_failing_tool_fails_the_run_but_the_rest_still_run(tmp_path, fake_bin, monkeypatch):
    monkeypatch.setattr(run_mod.shutil, "which", lambda name: None)
    monkeypatch.setenv("FAKE_EXIT_MEMBW", "1")
    out = tmp_path / "run"
    assert run_mod.command(run_args(out, fake_bin)) == 1
    manifest = results.load(out / "run.json")
    codes = {t["tool"]: t["exit_code"] for t in manifest["results"]["tools"]}
    assert codes["membw"] == 1
    assert codes["memtest"] == 0
    assert "membw" not in manifest["results"]["files"]


def test_memtest_errors_make_the_run_exit_2(tmp_path, fake_bin, monkeypatch):
    monkeypatch.setattr(run_mod.shutil, "which", lambda name: None)
    monkeypatch.setenv("FAKE_EXIT_MEMTEST", "2")
    assert run_mod.command(run_args(tmp_path / "run", fake_bin)) == 2


def test_missing_tool_is_reported(tmp_path, fake_bin, monkeypatch):
    monkeypatch.setattr(run_mod.shutil, "which", lambda name: None)
    (fake_bin / "perfcheck").unlink()
    monkeypatch.setattr(run_mod.tools.shutil, "which", lambda name: None)
    monkeypatch.setattr(run_mod.tools, "repo_root", lambda: tmp_path)
    out = tmp_path / "run"
    assert run_mod.command(run_args(out, fake_bin)) == 1
    codes = {t["tool"]: t["exit_code"] for t in results.load(out / "run.json")["results"]["tools"]}
    assert codes["perfcheck"] == 127


def test_without_perf_the_cross_check_says_why(tmp_path, fake_bin, monkeypatch):
    monkeypatch.setattr(run_mod.shutil, "which", lambda name: None)
    out = tmp_path / "run"
    run_mod.command(run_args(out, fake_bin))
    ps = load_json(out / "perfcheck.json")["results"]["perf_stat"]
    assert ps["available"] is False
    # The sample run comes from a VM without a PMU, so that reason wins.
    assert "unavailable" in ps["reason"] or "not installed" in ps["reason"]


PERF_CSV = """# started on Tue Oct 14 12:00:00 2026

1000000,,cycles:u,500000,100.00,,
2000000,,instructions:u,500000,100.00,2.00,insn per cycle
<not counted>,,branch-misses:u,0,0.00,,
"""


def test_parse_perf_stat_csv():
    counts = run_mod.parse_perf_stat_csv(PERF_CSV)
    assert counts == {"cycles": 1_000_000.0, "instructions": 2_000_000.0, "branch-misses": None}


def counters_doc(cycles, instructions):
    ev = lambda v: {"raw": int(v), "scaled": v, "time_enabled_ns": 1, "time_running_ns": 1, "scale": 1.0}  # noqa: E731
    return {
        "results": {
            "counters": {
                "available": True,
                "reason": None,
                "events": {"cycles": ev(cycles), "instructions": ev(instructions)},
            }
        }
    }


def test_cross_check_within_tolerance():
    check = run_mod.cross_check(counters_doc(990_000, 1_995_000), run_mod.parse_perf_stat_csv(PERF_CSV), None)
    assert check["available"] is True
    assert check["within_tolerance"] is True
    assert abs(check["events"]["cycles"]["diff_pct"] - (-1.0)) < 1e-9


def test_cross_check_outside_tolerance():
    check = run_mod.cross_check(counters_doc(900_000, 2_000_000), run_mod.parse_perf_stat_csv(PERF_CSV), None)
    assert check["within_tolerance"] is False


def test_cross_check_without_counters_or_perf(sample_doc):
    doc = sample_doc("perfcheck")
    assert "unavailable" in run_mod.cross_check(doc, None, "perf missing")["reason"]
    assert run_mod.cross_check(counters_doc(1, 1), None, "perf missing")["reason"] == "perf missing"


def test_numa_runs_only_with_two_or_more_nodes(tmp_path, fake_bin):
    one_node = {"results": {"numa": [{"node": 0, "cpus": [0, 1], "mem_total_kib": 100}]}}
    assert run_mod.run_numa(tmp_path, one_node, fake_bin, None) == []
    two = {
        "results": {
            "numa": [
                {"node": 0, "cpus": [0, 1], "mem_total_kib": 100},
                {"node": 1, "cpus": [2, 3], "mem_total_kib": 100},
            ]
        }
    }
    # The fake memlat copies memlat.json, which is fine for checking the calls.
    runs = run_mod.run_numa(tmp_path, two, fake_bin, None)
    assert len(runs) == 2
    calls = fake_bin.calls()
    assert [c[c.index("--mem-node") + 1] for c in calls] == ["0", "1"]
    assert all(c[c.index("--cpu") + 1] == "0" for c in calls)
