"""hwdiag inventory: run the inventory tool and print a readable summary."""

from __future__ import annotations

import json
import tempfile
from pathlib import Path
from typing import Any

from . import results, tools


def human_bytes(n: float) -> str:
    for unit in ("B", "K", "M", "G", "T"):
        if n < 1024 or unit == "T":
            return f"{n:g}{unit}" if float(n).is_integer() else f"{n:.1f}{unit}"
        n /= 1024
    return f"{n}"


def cache_line(caches: list[dict[str, Any]]) -> str:
    parts = []
    for c in caches:
        suffix = {"Data": "d", "Instruction": "i"}.get(c["type"], "")
        parts.append(f"L{c['level']}{suffix} {human_bytes(c['size_bytes'])}")
    line = ", ".join(parts) or "none in sysfs"
    sizes = {c.get("line_size") for c in caches if c.get("line_size")}
    if sizes:
        line += f" (line {', '.join(str(s) for s in sorted(sizes))} B)"
    return line


def summarize(doc: dict[str, Any]) -> str:
    """Multi-line text summary of an inventory result."""
    r = doc["results"]
    host = doc["host"]
    topo = r["topology"]
    freq = r["frequency"]
    mem = r["memory"]
    lines = [
        f"Host       {host['hostname']} (kernel {host['kernel']})",
        f"CPU        {r['cpu']['model']} ({r['cpu']['logical_cpus']} logical CPUs)",
        f"Topology   {topo['sockets']} socket(s), {topo['cores']} core(s), {topo['threads']} thread(s), "
        f"SMT {host['smt']}",
        f"Caches     {cache_line(r['caches'])}",
    ]
    if freq["available"]:
        cur = [c["cur_mhz"] for c in freq["per_cpu"] if c["cur_mhz"] is not None]
        span = f"{min(cur):.0f}-{max(cur):.0f} MHz now" if cur else "no current frequency"
        lines.append(f"Frequency  {span}, governor {freq['governor']}, turbo {host['turbo']}")
    else:
        lines.append("Frequency  cpufreq not available (normal in VMs)")
    lines.append(
        f"Memory     {human_bytes(mem['total_kib'] * 1024)} total, "
        f"{human_bytes(mem['available_kib'] * 1024)} available, {len(r['numa'])} NUMA node(s)"
    )
    with_link = [d for d in r["pcie"] if d["link"]]
    lines.append(f"PCIe       {len(r['pcie'])} device(s), {len(with_link)} with link status")
    for d in with_link:
        link = d["link"]
        cur = f"{link['cur_speed_gts']:g} GT/s x{link['cur_width']}" if link["cur_speed_gts"] else "unknown"
        mx = f"{link['max_speed_gts']:g} GT/s x{link['max_width']}" if link["max_speed_gts"] else "unknown"
        lines.append(f"             {d['address']} {d['class_name']}: {cur} (max {mx})")
    temps = [s for s in r["sensors"] if s["kind"] == "temperature"]
    if temps:
        hottest = max(temps, key=lambda s: s["value"])
        lines.append(
            f"Sensors    {len(temps)} temperature(s), hottest {hottest['chip']}/{hottest['label']} "
            f"{hottest['value']:.1f} C"
        )
    else:
        lines.append("Sensors    none")
    edac = r["edac"]
    if edac["present"]:
        ce = sum(c["ce_count"] for c in edac["controllers"])
        ue = sum(c["ue_count"] for c in edac["controllers"])
        lines.append(f"EDAC       {len(edac['controllers'])} controller(s), {ce} corrected, {ue} uncorrected")
    else:
        lines.append("EDAC       not present")
    lines.append("Checks")
    for c in r["checks"]:
        lines.append(f"  [{c['level']:>4}] {c['check']} {c['subject']}: {c['message']}")
    lines.append(f"Status     {r['status']}")
    return "\n".join(lines)


def command(args) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(args.out) if args.out else Path(tmp) / "inventory.json"
        tool_args = ["--root", args.root, "--temp-threshold", str(args.temp_threshold)]
        env = {"HWDIAG_GIT_COMMIT": c} if (c := tools.git_commit()) else None
        run = tools.run_tool("inventory", tool_args, out, args.bin_dir, env=env)
        if run.out is None or run.exit_code not in (0, 2):
            print(f"hwdiag: inventory failed (exit code {run.exit_code})")
            return 1
        doc = results.load(run.out)
        print(json.dumps(doc, indent=2) if args.json else summarize(doc))
        return 2 if doc["results"]["status"] == "fail" else 0
