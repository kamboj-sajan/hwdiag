"""hwdiag compare: % change per metric between two runs; exit 1 on a regression."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from typing import Any

from . import metrics as metrics_mod
from .metrics import Metric


@dataclass
class Row:
    name: str
    unit: str
    better: str
    base: float
    new: float
    change_pct: float | None  # (new - base) / base * 100
    worse_pct: float | None  # change in the "worse" direction (positive = worse)
    status: str  # ok, improved, regression
    noise_pct: float | None  # combined run-to-run spread of base and new


def compare_metrics(base: dict[str, Metric], new: dict[str, Metric], tolerance: float) -> list[Row]:
    rows = []
    for name in sorted(base.keys() & new.keys()):
        b, n = base[name], new[name]
        change = (n.value - b.value) / b.value * 100.0 if b.value else None
        worse_raw = n.value - b.value if b.better == "lower" else b.value - n.value
        worse = None if change is None else (change if b.better == "lower" else -change)
        if b.absolute or worse is None:
            # Counts like memtest errors: any change for the worse is a regression.
            status = "regression" if worse_raw > 0 else ("improved" if worse_raw < 0 else "ok")
        elif worse > tolerance:
            status = "regression"
        elif worse < -tolerance:
            status = "improved"
        else:
            status = "ok"
        noise = None
        if b.spread_pct is not None and n.spread_pct is not None:
            noise = b.spread_pct + n.spread_pct
        rows.append(Row(name, b.unit, b.better, b.value, n.value, change, worse, status, noise))
    return rows


def fmt_value(v: float, unit: str) -> str:
    if unit == "errors":
        return f"{v:.0f}"
    return f"{v:.2f}" if abs(v) < 100 else f"{v:.1f}"


def render_table(rows: list[Row]) -> str:
    header = ("metric", "base", "new", "change", "spread", "status")
    lines = [header]
    for r in rows:
        change = "n/a" if r.change_pct is None else f"{r.change_pct:+.1f}%"
        noise = "" if r.noise_pct is None else f"{r.noise_pct:.0f}%"
        status = r.status.upper() if r.status == "regression" else r.status
        lines.append((r.name, fmt_value(r.base, r.unit), fmt_value(r.new, r.unit), change, noise, status))
    widths = [max(len(row[i]) for row in lines) for i in range(len(header))]
    out = []
    for i, row in enumerate(lines):
        cells = [row[0].ljust(widths[0])] + [c.rjust(w) for c, w in zip(row[1:], widths[1:], strict=True)]
        out.append("  ".join(cells))
        if i == 0:
            out.append("  ".join("-" * w for w in widths))
    return "\n".join(out)


def cpu_models(docs: list[dict[str, Any]]) -> set[str]:
    return {d["host"]["cpu_model"] for d in docs}


def command(args) -> int:
    base, base_docs = metrics_mod.load_set(args.base)
    new, new_docs = metrics_mod.load_set(args.new)
    rows = compare_metrics(base, new, args.tolerance)
    regressions = [r for r in rows if r.status == "regression"]
    base_cpus, new_cpus = cpu_models(base_docs), cpu_models(new_docs)
    warnings = []
    if base_cpus != new_cpus:
        warnings.append(
            f"different CPUs: base {sorted(base_cpus)} vs new {sorted(new_cpus)}; "
            "differences may be the hardware, not a regression"
        )
    only_base = sorted(base.keys() - new.keys())
    only_new = sorted(new.keys() - base.keys())

    if args.json:
        print(
            json.dumps(
                {
                    "tolerance_pct": args.tolerance,
                    "rows": [asdict(r) for r in rows],
                    "regressions": [r.name for r in regressions],
                    "only_in_base": only_base,
                    "only_in_new": only_new,
                    "warnings": warnings,
                },
                indent=2,
            )
        )
    else:
        for w in warnings:
            print(f"warning: {w}")
        if rows:
            print(render_table(rows))
        else:
            print("no metrics in common")
        if only_base:
            print(f"\nonly in base: {', '.join(only_base)}")
        if only_new:
            print(f"only in new: {', '.join(only_new)}")
        print(
            f"\n{len(rows)} metric(s) compared, tolerance {args.tolerance:g}%: "
            f"{len(regressions)} regression(s)"
        )
    return 1 if regressions else 0
