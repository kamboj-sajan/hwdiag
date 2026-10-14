"""Headline metrics extracted from results, for compare and report."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import results


@dataclass
class Metric:
    name: str
    value: float
    better: str  # "lower" or "higher"
    unit: str
    absolute: bool = False  # any change in the worse direction is a regression
    spread_pct: float | None = None  # run-to-run spread, when the tool measured one


def cache_levels(sysfs_caches: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Data and unified caches, one per level, smallest level first."""
    by_level: dict[int, dict[str, Any]] = {}
    for c in sysfs_caches:
        if c["type"] != "Instruction":
            by_level.setdefault(c["level"], c)
    return [by_level[k] for k in sorted(by_level)]


def level_points(curve: dict[str, Any], sysfs_caches: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """The representative point for each cache level and DRAM. A level's point
    is the largest size at most half its sysfs size (well inside the level,
    away from the transition); DRAM is the largest size measured."""
    pts = sorted(curve["points"], key=lambda p: p["size_bytes"])
    out: dict[str, dict[str, Any]] = {}
    for c in cache_levels(sysfs_caches):
        inside = [p for p in pts if p["size_bytes"] <= c["size_bytes"] // 2]
        # Skip a level whose point would be the largest size: that point is
        # the DRAM figure (the run stopped before the level's half size).
        if inside and (len(inside) < len(pts) or len(pts) == 1):
            out[f"L{c['level']}"] = inside[-1]
    if pts:
        out["dram"] = pts[-1]
    return out


def extract(doc: dict[str, Any]) -> list[Metric]:
    tool = doc["tool"]
    r = doc["results"]
    out: list[Metric] = []
    if tool == "memlat":
        for curve in r["curves"]:
            for level, p in level_points(curve, r["sysfs_caches"]).items():
                out.append(
                    Metric(
                        f"memlat.{curve['name']}.{level}_ns",
                        p["median_ns"],
                        "lower",
                        "ns",
                        spread_pct=p["spread_pct"],
                    )
                )
    elif tool == "membw" and r.get("mode") == "sweep":
        for run in r["runs"]:
            for kernel, k in run["kernels"].items():
                out.append(
                    Metric(
                        f"membw.{kernel}.t{run['threads']}_gbs",
                        k["best_gbs"],
                        "higher",
                        "GB/s",
                        spread_pct=k.get("spread_pct"),
                    )
                )
    elif tool == "memtest":
        out.append(Metric("memtest.total_errors", float(r["total_errors"]), "lower", "errors", absolute=True))
    return out


def load_set(path: str | Path) -> tuple[dict[str, Metric], list[dict[str, Any]]]:
    """Metrics from a run directory, a run.json, or a single result file."""
    p = Path(path)
    if p.is_file() and p.name == "run.json":
        p = p.parent
    files = sorted(f for f in p.glob("*.json") if f.name != "run.json") if p.is_dir() else [p]
    if not files:
        raise results.ResultError(f"no result files in {p}")
    docs = [results.load(f) for f in files]
    metrics: dict[str, Metric] = {}
    for d in docs:
        for m in extract(d):
            metrics[m.name] = m
    return metrics, docs
