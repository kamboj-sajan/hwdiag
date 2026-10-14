from __future__ import annotations

import argparse
import json
import shutil

import pytest
from conftest import SAMPLE_RUN
from hwdiag import compare, metrics, results


def copy_run(tmp_path, name="run"):
    dst = tmp_path / name
    shutil.copytree(SAMPLE_RUN, dst)
    return dst


def edit(run_dir, tool, fn):
    path = run_dir / f"{tool}.json"
    doc = json.loads(path.read_text())
    fn(doc)
    results.save(doc, path)


def cmp_args(base, new, tolerance=5.0, as_json=False):
    return argparse.Namespace(base=str(base), new=str(new), tolerance=tolerance, json=as_json)


def scale_memlat_dram(factor):
    def fn(doc):
        for curve in doc["results"]["curves"]:
            p = curve["points"][-1]
            for key in ("median_ns", "min_ns", "max_ns"):
                p[key] *= factor

    return fn


def scale_triad(factor):
    def fn(doc):
        for run in doc["results"]["runs"]:
            run["kernels"]["triad"]["best_gbs"] *= factor

    return fn


def test_a_run_compared_with_itself_has_no_regressions(capsys):
    assert compare.command(cmp_args(SAMPLE_RUN, SAMPLE_RUN)) == 0
    out = capsys.readouterr().out
    assert "0 regression(s)" in out
    assert "memlat.random/4k.dram_ns" in out


def test_slower_dram_latency_is_a_regression(tmp_path, capsys):
    new = copy_run(tmp_path)
    edit(new, "memlat", scale_memlat_dram(1.10))
    assert compare.command(cmp_args(SAMPLE_RUN, new, tolerance=5)) == 1
    out = capsys.readouterr().out
    assert "REGRESSION" in out
    assert "+10.0%" in out
    # The same change passes with a looser tolerance.
    assert compare.command(cmp_args(SAMPLE_RUN, new, tolerance=15)) == 0


def test_lower_bandwidth_is_a_regression_and_higher_is_an_improvement(tmp_path):
    slow = copy_run(tmp_path, "slow")
    edit(slow, "membw", scale_triad(0.9))
    rows = compare.compare_metrics(metrics.load_set(SAMPLE_RUN)[0], metrics.load_set(slow)[0], 5)
    triad = [r for r in rows if r.name.startswith("membw.triad")]
    assert triad and all(r.status == "regression" for r in triad)
    assert all(abs(r.worse_pct - 10.0) < 1e-6 for r in triad)

    fast = copy_run(tmp_path, "fast")
    edit(fast, "membw", scale_triad(1.2))
    rows = compare.compare_metrics(metrics.load_set(SAMPLE_RUN)[0], metrics.load_set(fast)[0], 5)
    assert all(r.status == "improved" for r in rows if r.name.startswith("membw.triad"))


def test_small_changes_inside_the_tolerance_are_ok(tmp_path):
    new = copy_run(tmp_path)
    edit(new, "memlat", scale_memlat_dram(1.03))
    assert compare.command(cmp_args(SAMPLE_RUN, new, tolerance=5)) == 0


def test_any_new_memtest_error_is_a_regression_whatever_the_tolerance(tmp_path):
    new = copy_run(tmp_path)

    def add_error(doc):
        doc["results"]["total_errors"] = 1
        doc["results"]["status"] = "fail"

    edit(new, "memtest", add_error)
    assert compare.command(cmp_args(SAMPLE_RUN, new, tolerance=1000)) == 1


def test_json_output(tmp_path, capsys):
    new = copy_run(tmp_path)
    edit(new, "memlat", scale_memlat_dram(1.5))
    assert compare.command(cmp_args(SAMPLE_RUN, new, as_json=True)) == 1
    out = json.loads(capsys.readouterr().out)
    assert out["tolerance_pct"] == 5.0
    assert "memlat.random/4k.dram_ns" in out["regressions"]
    row = next(r for r in out["rows"] if r["name"] == "memlat.random/4k.dram_ns")
    assert row["better"] == "lower"
    assert row["change_pct"] == pytest.approx(50.0)


def test_different_cpus_are_flagged(tmp_path, capsys):
    new = copy_run(tmp_path)
    for tool in ("memlat", "membw", "memtest", "inventory", "perfcheck"):
        edit(new, tool, lambda d: d["host"].update(cpu_model="Some Other CPU"))
    compare.command(cmp_args(SAMPLE_RUN, new))
    assert "different CPUs" in capsys.readouterr().out


def test_single_files_and_run_json_paths_work(capsys):
    assert compare.command(cmp_args(SAMPLE_RUN / "membw.json", SAMPLE_RUN / "run.json")) == 0
    out = capsys.readouterr().out
    assert "only in new" in out  # memlat and memtest metrics exist only on the run side


def test_level_points_take_half_the_sysfs_size():
    doc = results.load(SAMPLE_RUN / "memlat.json")
    curve = doc["results"]["curves"][0]
    lp = metrics.level_points(curve, doc["results"]["sysfs_caches"])
    caches = {
        c["level"]: c["size_bytes"] for c in doc["results"]["sysfs_caches"] if c["type"] != "Instruction"
    }
    assert lp["L1"]["size_bytes"] <= caches[1] // 2
    assert lp["L2"]["size_bytes"] <= caches[2] // 2
    assert lp["dram"]["size_bytes"] == max(p["size_bytes"] for p in curve["points"])
    if caches[3] // 2 >= lp["dram"]["size_bytes"]:
        assert "L3" not in lp  # would duplicate the DRAM point


def test_missing_results_raise(tmp_path):
    with pytest.raises(results.ResultError):
        metrics.load_set(tmp_path)


def test_table_rendering_lines_up():
    rows = [
        compare.Row("memlat.x.L1_ns", "ns", "lower", 1.0, 1.2, 20.0, 20.0, "regression", 5.0),
        compare.Row("membw.triad.t1_gbs", "GB/s", "higher", 10.0, 10.0, 0.0, -0.0, "ok", None),
    ]
    lines = compare.render_table(rows).splitlines()
    assert len({len(line) for line in lines}) == 1
    assert "REGRESSION" in lines[2]
