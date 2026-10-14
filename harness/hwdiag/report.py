"""hwdiag report: plots and a Markdown report for one run directory."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from . import plots, results
from .inventory import cache_line, human_bytes
from .metrics import level_points

TOOLS = ("inventory", "memlat", "membw", "memtest", "perfcheck")


def load_run(path: str | Path) -> tuple[Path, dict[str, dict[str, Any]], dict[int, dict[str, Any]]]:
    p = Path(path)
    run_dir = p.parent if p.is_file() else p
    if not run_dir.is_dir():
        raise results.ResultError(f"{path} is not a run directory")
    docs = {}
    for tool in (*TOOLS, "run", "stress"):
        f = run_dir / f"{tool}.json"
        if f.exists():
            docs[tool] = results.load(f)
    if not any(t in docs for t in TOOLS):
        raise results.ResultError(f"no tool results in {run_dir}")
    numa = {}
    for f in sorted((run_dir / "numa").glob("memlat_node*.json")):
        doc = results.load(f)
        numa[int(doc["params"]["mem_node"])] = doc
    return run_dir, docs, numa


def fmt(v: float | None, digits: int = 2, unit: str = "") -> str:
    if v is None:
        return "n/a"
    return f"{v:.{digits}f}{unit}"


def table(header: list[str], rows: list[list[str]]) -> list[str]:
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(r) + " |" for r in rows]
    return out


def host_section(doc: dict[str, Any]) -> list[str]:
    h = doc["host"]
    return [
        f"- **CPU:** {h['cpu_model']} ({h['logical_cpus']} logical CPUs)",
        f"- **Kernel:** {h['kernel']} · **governor:** {h['governor'] or 'n/a (no cpufreq)'} · "
        f"**turbo:** {h['turbo']} · **SMT:** {h['smt']}",
        f"- **Compiler:** {h['compiler']} `{h['compiler_flags']}` ({h['build_type']})",
        f"- **Commit:** `{doc['git_commit']}` · **measured:** {doc['timestamp']}",
    ]


def inventory_section(inv: dict[str, Any]) -> list[str]:
    r = inv["results"]
    topo = r["topology"]
    out = ["## Inventory", ""]
    out += table(
        ["item", "value"],
        [
            ["CPU", r["cpu"]["model"]],
            [
                "Topology",
                f"{topo['sockets']} socket(s), {topo['cores']} core(s), {topo['threads']} thread(s)",
            ],
            ["Caches", cache_line(r["caches"])],
            ["Memory", f"{human_bytes(r['memory']['total_kib'] * 1024)}, {len(r['numa'])} NUMA node(s)"],
            ["PCIe devices", f"{len(r['pcie'])} ({sum(1 for d in r['pcie'] if d['link'])} with link status)"],
            ["Sensors", str(len(r["sensors"]))],
            ["EDAC", "present" if r["edac"]["present"] else "not present"],
            ["Status", r["status"]],
        ],
    )
    out += ["", "**Checks**", ""]
    problems = [c for c in r["checks"] if c["level"] in ("warn", "fail")]
    for c in r["checks"]:
        out.append(f"- `{c['level']}` **{c['check']}** {c['subject']}: {c['message']}")
    if not problems:
        out.append("- No inventory warnings.")
    return out + [""]


def memlat_section(lat: dict[str, Any], run_dir: Path, out_dir: Path) -> list[str]:
    r = lat["results"]
    out = ["## Memory latency (memlat)", ""]
    plots.latency_curve(lat, out_dir / "latency.png")
    out += [
        "![Latency vs buffer size](latency.png)",
        "",
        "Pointer chasing over a random cycle of 64-byte lines (Sattolo's algorithm), one pinned core, "
        f"median of {lat['params']['reps']} repetitions. Vertical lines: cache sizes from sysfs. "
        "Circles: detected boundaries (the last size before latency rose more than "
        f"{lat['params']['threshold']:g}x).",
        "",
    ]
    if plots.latency_modes(lat, out_dir / "latency_modes.png"):
        out += ["![Latency by pattern and page size](latency_modes.png)", ""]

    names = [c["name"] for c in r["curves"]]
    by_curve = {c["name"]: {p["size_bytes"]: p for p in c["points"]} for c in r["curves"]}
    sizes = sorted({s for c in by_curve.values() for s in c})
    rows = []
    for s in sizes:
        row = [plots.size_label(s)]
        for n in names:
            p = by_curve[n].get(s)
            row.append(f"{p['median_ns']:.2f} ({p['spread_pct']:.0f}%)" if p else "")
        rows.append(row)
    out += ["<details><summary>Latency table: median ns per load (spread)</summary>", ""]
    out += table(["size", *names], rows)
    out += ["", "</details>", ""]

    out += ["**Representative latencies** (point at half of each sysfs cache size; DRAM = largest size)", ""]
    rows = []
    for c in r["curves"]:
        lp = level_points(c, r["sysfs_caches"])
        rows.append(
            [c["name"]]
            + [
                f"{lp[k]['median_ns']:.2f} ns @ {plots.size_label(lp[k]['size_bytes'])}" if k in lp else "n/a"
                for k in ("L1", "L2", "L3", "dram")
            ]
        )
    out += table(["curve", "L1", "L2", "L3", "DRAM"], rows)
    out += ["", f"**Detected cache boundaries** (from {r['detected_from'] or 'n/a'})", ""]
    rows = [
        [
            plots.size_label(m["detected_bytes"]),
            f"L{m['level']}" if m["level"] else "unmatched",
            plots.size_label(m["sysfs_bytes"]) if m["sysfs_bytes"] else "n/a",
            fmt(m["ratio"], 2),
        ]
        for m in r["detected_caches"]
    ]
    out += table(["detected", "matched level", "sysfs size", "detected / sysfs"], rows) if rows else ["None."]
    out.append("")
    return out


def bandwidth_section(bw: dict[str, Any], out_dir: Path) -> list[str]:
    r = bw["results"]
    out = ["## Memory bandwidth (membw)", ""]
    if plots.bandwidth(bw, out_dir / "bandwidth.png"):
        out += ["![Bandwidth vs threads](bandwidth.png)", ""]
    out += [
        f"STREAM kernels on 3 arrays of {plots.size_label(bw['params']['array_bytes'])} "
        f"(last-level cache {plots.size_label(bw['params']['llc_bytes'])}), "
        f"{bw['params']['reps']} repetitions. "
        "GB/s counts bytes the STREAM way; copy also reads each destination line before writing it "
        "(write-allocate), which is not counted. "
        f"Validated against the STREAM recurrence: **{'yes' if r['validated'] else 'NO'}**.",
        "",
    ]
    rows = []
    for run in r["runs"]:
        rows.append(
            [str(run["threads"])]
            + [
                f"{run['kernels'][k]['best_gbs']:.1f} / {run['kernels'][k]['median_gbs']:.1f}"
                for k in ("copy", "scale", "add", "triad")
            ]
        )
    out += table(["threads", "copy", "scale", "add", "triad"], rows)
    out += ["", "Cells: best / median GB/s.", ""]
    return out


def counters_section(docs: dict[str, dict[str, Any]]) -> list[str]:
    out = ["## Hardware counters (perfctr)", ""]
    lat = docs.get("memlat")
    if lat and lat["results"]["counters"]["available"]:
        layout = lat["results"]["counters"].get("group_layout", [])
        out += [f"Counter groups: {' | '.join(', '.join(g) for g in layout)}", ""]
        curve = next(
            (c for c in lat["results"]["curves"] if c["name"] == "random/4k"), lat["results"]["curves"][0]
        )
        rows = []
        for p in curve["points"]:
            c = p["counters"]
            if not c:
                continue
            pl, d = c.get("per_load", {}), c["derived"]
            rows.append(
                [
                    plots.size_label(p["size_bytes"]),
                    fmt(d.get("ipc"), 3),
                    fmt(pl.get("cycles"), 1),
                    fmt(pl.get("l1d_read_misses"), 3),
                    fmt(pl.get("dtlb_read_misses"), 3),
                    fmt(d.get("cache_misses_pki"), 1),
                    "yes" if c["multiplexed"] else "no",
                ]
            )
        out += [f"memlat {curve['name']}, per load:", ""]
        out += table(
            ["size", "IPC", "cycles", "L1D misses", "dTLB misses", "cache misses / 1k instr", "multiplexed"],
            rows,
        )
        out.append("")
    elif lat:
        out += [f"Hardware counters were unavailable: {lat['results']['counters']['reason']}", ""]
    pc = docs.get("perfcheck")
    if pc:
        ps = pc["results"].get("perf_stat")
        out += ["**Cross-check against perf stat** (perfcheck workload, same process)", ""]
        if ps and ps["available"]:
            rows = [
                [name, fmt(e["perf_stat"], 0), fmt(e["perfctr"], 0), fmt(e["diff_pct"], 2, "%")]
                for name, e in ps["events"].items()
            ]
            out += table(["event", "perf stat", "perfctr", "difference"], rows)
            ok = "within" if ps["within_tolerance"] else "OUTSIDE"
            out += ["", f"{ok} the {ps['tolerance_pct']:g}% tolerance.", ""]
        else:
            out += [f"Not compared: {ps['reason'] if ps else 'not run'}", ""]
    return out


def memtest_section(mt: dict[str, Any]) -> list[str]:
    r = mt["results"]
    out = [
        "## Memory test (memtest)",
        "",
        f"{plots.size_label(r['bytes_tested'])} tested, "
        f"{'locked with mlock' if r['locked'] else 'NOT locked'}, "
        f"{r['total_errors']} error(s). Status: **{r['status']}**.",
        "",
    ]
    rows = [
        [p["name"], str(p["pass"]), str(p["errors"]), fmt(p["seconds"], 2), fmt(p["gbs"], 1)]
        for p in r["patterns"]
    ]
    out += table(["pattern", "pass", "errors", "seconds", "GB/s"], rows)
    if r["errors"]:
        out += ["", "First errors:", ""]
        rows = [
            [
                e["pattern"],
                e["offset_hex"],
                e["expected"],
                e["actual"],
                e["xor_mask"],
                ",".join(str(b) for b in e["flipped_bits"]),
            ]
            for e in r["errors"][:10]
        ]
        out += table(["pattern", "offset", "expected", "actual", "xor", "bits"], rows)
    return out + [""]


def numa_section(numa: dict[int, dict[str, Any]], out_dir: Path) -> list[str]:
    if len(numa) < 2:
        return []
    cpu = next(iter(numa.values()))["params"]["cpu"]
    local = 0
    out = ["## NUMA: local vs remote latency", ""]
    if plots.numa(numa, local, out_dir / "numa.png"):
        out += ["![NUMA latency](numa.png)", ""]
    sizes = [p["size_bytes"] for p in numa[min(numa)]["results"]["curves"][0]["points"]]
    rows = []
    for node, doc in sorted(numa.items()):
        pts = {p["size_bytes"]: p["median_ns"] for p in doc["results"]["curves"][0]["points"]}
        rows.append([str(node)] + [fmt(pts.get(s), 1) for s in sizes])
    out += [f"CPU {cpu} (node {local}), memory bound to each node; median ns per load:", ""]
    out += table(["memory node", *[plots.size_label(s) for s in sizes]], rows)
    return out + [""]


def build(run_path: str | Path, out_dir: str | Path | None = None) -> Path:
    run_dir, docs, numa = load_run(run_path)
    out = Path(out_dir) if out_dir else run_dir
    out.mkdir(parents=True, exist_ok=True)
    first = docs.get("inventory") or next(d for t, d in docs.items() if t in TOOLS)
    suite = docs["run"]["results"]["suite"] if "run" in docs else "n/a"
    lines = [
        f"# hwdiag report: {first['host']['hostname']}",
        "",
        f"Suite: {suite} · run directory: `{run_dir}`",
        "",
    ]
    lines += host_section(first) + [""]
    if "run" in docs:
        s = docs["run"]["results"]["summary"]
        rows = []
        if s.get("dram_latency_ns") is not None:
            rows.append(
                [
                    "DRAM latency (random, 4 KiB pages)",
                    f"{s['dram_latency_ns']:.1f} ns at {plots.size_label(s['dram_size_bytes'])}",
                    "memlat.json",
                ]
            )
        if "peak_bandwidth_gbs" in s:
            rows.append(
                [
                    "Peak bandwidth",
                    f"{s['peak_bandwidth_gbs']:.1f} GB/s ({s['peak_kernel']}, {s['peak_threads']} threads)",
                    "membw.json",
                ]
            )
        if "memtest_errors" in s:
            rows.append(
                [
                    "memtest errors",
                    f"{s['memtest_errors']} in {plots.size_label(s['memtest_bytes'])}",
                    "memtest.json",
                ]
            )
        if "counters_available" in s:
            rows.append(
                [
                    "Hardware counters",
                    "available" if s["counters_available"] else "unavailable",
                    "memlat.json",
                ]
            )
        if "inventory_status" in s:
            rows.append(["Inventory status", s["inventory_status"], "inventory.json"])
        lines += ["## Summary", ""] + table(["measure", "value", "source"], rows) + [""]
    if "inventory" in docs:
        lines += inventory_section(docs["inventory"])
    if "memlat" in docs:
        lines += memlat_section(docs["memlat"], run_dir, out)
    if "membw" in docs:
        lines += bandwidth_section(docs["membw"], out)
    if "memlat" in docs or "perfcheck" in docs:
        lines += counters_section(docs)
    if "memtest" in docs:
        lines += memtest_section(docs["memtest"])
    lines += numa_section(numa, out)
    warnings = sorted({w for d in docs.values() for w in d["warnings"]})
    lines += ["## Warnings", ""] + ([f"- {w}" for w in warnings] or ["- None."]) + [""]
    report = out / "REPORT.md"
    report.write_text("\n".join(lines))
    return report


def command(args) -> int:
    report = build(args.run, args.out)
    print(f"hwdiag: wrote {report}")
    return 0
