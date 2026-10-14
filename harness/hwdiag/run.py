"""hwdiag run: run a suite of tools into one run directory."""

from __future__ import annotations

import json
import re
import shutil
import socket
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from . import results, tools

# Arguments per tool for each suite. memlat and membw are pinned and repeat
# every measurement; the quick suite keeps a CI run to about a minute.
SUITES: dict[str, dict[str, list[str]]] = {
    "quick": {
        "inventory": [],
        "memlat": ["--max", "64M", "--reps", "5", "--target-ms", "20", "--modes", "all"],
        "membw": ["--reps", "3"],
        "memtest": ["--size", "64M"],
        "perfcheck": ["--alu-iterations", "50000000", "--chase-steps", "1000000"],
    },
    "full": {
        "inventory": [],
        "memlat": ["--max", "1G", "--reps", "7", "--target-ms", "100", "--modes", "all"],
        "membw": ["--reps", "10"],
        "memtest": [],  # default: 25% of MemAvailable
        "perfcheck": [],
    },
}

NUMA_SIZES = "64M,256M,1G"
PERF_TOLERANCE_PCT = 5.0


def default_run_dir(base: Path | None = None) -> Path:
    host = re.sub(r"[^A-Za-z0-9._-]", "_", socket.gethostname()) or "host"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return (base or tools.repo_root() / "results") / f"{host}-{stamp}"


def parse_perf_stat_csv(text: str) -> dict[str, float | None]:
    """Parses `perf stat -x,` output into {event: count}. Events that perf
    could not count come back as None. The ':u' modifier is dropped."""
    counts: dict[str, float | None] = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split(",")
        if len(fields) < 3:
            continue
        value, event = fields[0].strip(), fields[2].strip()
        event = event.split(":")[0]
        try:
            counts[event] = float(value)
        except ValueError:
            counts[event] = None  # "<not counted>" or "<not supported>"
    return counts


def cross_check(doc: dict[str, Any], perf_counts: dict[str, float | None] | None, reason: str | None) -> dict:
    """Compares perfctr's own counts with perf stat's for the same process."""
    ctr = doc["results"]["counters"]
    if not ctr["available"]:
        return {"available": False, "reason": f"perfctr counters unavailable: {ctr['reason']}"}
    if perf_counts is None:
        return {"available": False, "reason": reason or "perf stat not run"}
    events: dict[str, Any] = {}
    ok = True
    for name in ("cycles", "instructions"):
        ours = ctr["events"].get(name, {}).get("scaled")
        theirs = perf_counts.get(name)
        diff = (ours - theirs) / theirs * 100.0 if ours is not None and theirs else None
        events[name] = {"perf_stat": theirs, "perfctr": ours, "diff_pct": diff}
        ok = ok and diff is not None and abs(diff) <= PERF_TOLERANCE_PCT
    counted = any(e["diff_pct"] is not None for e in events.values())
    return {
        "available": counted,
        "reason": None if counted else "perf stat did not count cycles/instructions",
        "events": events,
        "tolerance_pct": PERF_TOLERANCE_PCT,
        "within_tolerance": ok if counted else None,
    }


def run_perfcheck(run_dir: Path, args: list[str], bin_dir, env) -> tools.ToolRun:
    """Runs perfcheck under perf stat when perf is installed, then records
    the cross-check in perfcheck.json."""
    out = run_dir / "perfcheck.json"
    stat_file = run_dir / "perf_stat.csv"
    perf = shutil.which("perf")
    reason = None
    perf_counts = None
    run = None
    if perf:
        wrapper = [perf, "stat", "-x,", "-e", "cycles:u,instructions:u", "-o", str(stat_file), "--"]
        run = tools.run_tool("perfcheck", args, out, bin_dir, env=env, wrapper=wrapper)
        if run.out is None or not stat_file.exists():
            reason = f"perf stat could not run perfcheck (exit code {run.exit_code})"
            run = None
        else:
            perf_counts = parse_perf_stat_csv(stat_file.read_text())
    else:
        reason = "perf is not installed (sudo apt install linux-tools-generic)"
    if run is None:
        run = tools.run_tool("perfcheck", args, out, bin_dir, env=env)
    if run.out is not None and run.exit_code == 0:
        doc = json.loads(run.out.read_text())
        doc["results"]["perf_stat"] = cross_check(doc, perf_counts, reason)
        results.save(doc, run.out)
    return run


def run_numa(run_dir: Path, inventory_doc: dict[str, Any], bin_dir, env) -> list[tools.ToolRun]:
    """Local vs remote memory latency: memlat pinned to node 0's first CPU
    with memory bound to each node in turn."""
    nodes = [n for n in inventory_doc["results"]["numa"] if n["cpus"] or n["mem_total_kib"]]
    if len(nodes) < 2 or not nodes[0]["cpus"]:
        return []
    cpu = nodes[0]["cpus"][0]
    runs = []
    for node in nodes:
        if not node["mem_total_kib"]:
            continue
        out = run_dir / "numa" / f"memlat_node{node['node']}.json"
        out.parent.mkdir(exist_ok=True)
        args = [
            "--sizes",
            NUMA_SIZES,
            "--pattern",
            "random",
            "--pages",
            "huge",
            "--reps",
            "5",
            "--cpu",
            str(cpu),
            "--mem-node",
            str(node["node"]),
        ]
        runs.append(tools.run_tool("memlat", args, out, bin_dir, env=env))
    return runs


def summarize(docs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    s: dict[str, Any] = {}
    if inv := docs.get("inventory"):
        r = inv["results"]
        s["cpu_model"] = r["cpu"]["model"]
        s["inventory_status"] = r["status"]
        s["inventory_warnings"] = [
            f"{c['check']} {c['subject']}: {c['message']}"
            for c in r["checks"]
            if c["level"] in ("warn", "fail")
        ]
    if lat := docs.get("memlat"):
        r = lat["results"]
        s["dram_latency_ns"] = r["dram_latency_ns"]
        s["dram_size_bytes"] = r.get("dram_size_bytes")
        s["detected_caches"] = r["detected_caches"]
        s["counters_available"] = r["counters"]["available"]
        s["counters_reason"] = r["counters"]["reason"]
    if bw := docs.get("membw"):
        r = bw["results"]
        s["peak_bandwidth_gbs"] = r["peak"]["gbs"]
        s["peak_kernel"] = r["peak"]["kernel"]
        s["peak_threads"] = r["peak"]["threads"]
        s["bandwidth_validated"] = r["validated"]
    if mt := docs.get("memtest"):
        r = mt["results"]
        s["memtest_errors"] = r["total_errors"]
        s["memtest_bytes"] = r["bytes_tested"]
        s["memtest_locked"] = r["locked"]
    if pc := docs.get("perfcheck"):
        ps = pc["results"].get("perf_stat", {})
        s["perf_stat_within_tolerance"] = ps.get("within_tolerance")
        s["perf_stat_reason"] = ps.get("reason")
    return s


def command(args) -> int:
    suite = SUITES[args.suite]
    run_dir = Path(args.out) if args.out else default_run_dir()
    run_dir.mkdir(parents=True, exist_ok=True)
    commit = tools.git_commit()
    env = {"HWDIAG_GIT_COMMIT": commit} if commit else None
    print(f"hwdiag: running the {args.suite} suite into {run_dir}")

    tool_runs: list[tools.ToolRun] = []
    docs: dict[str, dict[str, Any]] = {}
    files: dict[str, str] = {}
    failed = False
    problem = False
    for name, tool_args in suite.items():
        print(f"hwdiag: {name} ...", flush=True)
        if name == "perfcheck":
            run = run_perfcheck(run_dir, tool_args, args.bin_dir, env)
        else:
            run = tools.run_tool(name, tool_args, run_dir / f"{name}.json", args.bin_dir, env=env)
        tool_runs.append(run)
        if run.out is None or run.exit_code not in (0, 2):
            print(f"hwdiag: {name} failed (exit code {run.exit_code})")
            failed = True
            continue
        problem |= run.exit_code == 2
        try:
            docs[name] = results.load(run.out)
        except results.ResultError as e:
            print(f"hwdiag: {e}")
            failed = True
            continue
        files[name] = run.out.name

    if args.suite == "full" and "inventory" in docs:
        for run in run_numa(run_dir, docs["inventory"], args.bin_dir, env):
            tool_runs.append(run)
            if run.out is None or run.exit_code != 0:
                failed = True
                continue
            results.load(run.out)
            files[run.out.stem] = str(run.out.relative_to(run_dir))

    host = docs["inventory"]["host"] if "inventory" in docs else None
    manifest = results.make_result(
        "run",
        {"suite": args.suite, "out": str(run_dir)},
        {
            "suite": args.suite,
            "files": files,
            "tools": [
                {
                    "tool": r.tool,
                    "exit_code": r.exit_code,
                    "seconds": round(r.seconds, 3),
                    "file": str(r.out.relative_to(run_dir)) if r.out else None,
                }
                for r in tool_runs
            ],
            "summary": summarize(docs),
        },
        warnings=[w for d in docs.values() for w in d["warnings"]],
        host=host,
        git_commit=commit or "unknown",
    )
    results.save(manifest, run_dir / "run.json")

    s = manifest["results"]["summary"]
    print(f"hwdiag: wrote {run_dir / 'run.json'}")
    if "dram_latency_ns" in s and s["dram_latency_ns"] is not None:
        print(f"  DRAM latency     {s['dram_latency_ns']:.1f} ns at {s['dram_size_bytes'] >> 20} MiB")
    if "peak_bandwidth_gbs" in s:
        print(
            f"  Peak bandwidth   {s['peak_bandwidth_gbs']:.1f} GB/s ({s['peak_kernel']}, "
            f"{s['peak_threads']} threads)"
        )
    if "memtest_errors" in s:
        print(f"  memtest errors   {s['memtest_errors']}")
    if "counters_available" in s:
        print(f"  Counters         {'available' if s['counters_available'] else s['counters_reason']}")
    print(f"  Report:          hwdiag report {run_dir}")
    if failed:
        return 1
    return 2 if problem else 0
