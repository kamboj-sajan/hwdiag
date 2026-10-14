"""Charts for reports and stress runs (matplotlib, PNG).

Colours come from a validated categorical palette, assigned in fixed order:
the same curve or kernel always gets the same colour. Every chart has a
table twin in REPORT.md, and lines are labelled at their ends, so colour is
never the only way to tell series apart.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator  # noqa: E402

# Validated with the dataviz palette checker (light surface): adjacent CVD
# delta E >= 9.1, first three all-pairs >= 9.2.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"
STATUS_SERIOUS = "#ec835a"

CURVE_COLORS = {"random/4k": SERIES[0], "random/huge": SERIES[1], "sequential/4k": SERIES[2]}
KERNEL_COLORS = {"copy": SERIES[0], "scale": SERIES[1], "add": SERIES[2], "triad": SERIES[3]}


def size_label(n: float) -> str:
    for unit in ("B", "K", "M", "G", "T"):
        if n < 1024 or unit == "T":
            return f"{n:g}{unit}" if float(n).is_integer() else f"{n:.1f}{unit}"
        n /= 1024
    return str(n)


def _style(ax, title: str, xlabel: str, ylabel: str) -> None:
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", color=INK, fontsize=12, pad=10)
    ax.set_xlabel(xlabel, color=INK_2, fontsize=9)
    ax.set_ylabel(ylabel, color=INK_2, fontsize=9)
    ax.grid(True, which="major", color=GRID, linewidth=0.6, linestyle="-")
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(AXIS)
        ax.spines[side].set_linewidth(0.8)
    ax.tick_params(colors=MUTED, labelsize=8, length=3)


def _figure(width: float = 9.0, height: float = 4.8):
    fig, ax = plt.subplots(figsize=(width, height), dpi=130)
    fig.patch.set_facecolor(SURFACE)
    return fig, ax


def _size_axis(ax, sizes: list[int]) -> None:
    ax.set_xscale("log", base=2)
    ticks = [s for s in sizes if (s & (s - 1)) == 0]  # powers of two only
    step = max(1, len(ticks) // 10)
    ax.xaxis.set_major_locator(FixedLocator(ticks[::step]))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: size_label(v)))


def _end_labels(ax, fig, items: list[tuple[float, float, str, str]], fontsize: float = 8.0) -> None:
    """Labels at the ends of lines, nudged apart vertically so they never
    overlap. items: (x, y, text, colour)."""
    fig.canvas.draw()  # fixes the transforms for the final axis limits
    min_gap_px = fontsize * fig.dpi / 72.0 * 1.25  # one line of text plus a little air
    to_px = ax.transData.transform
    placed = sorted(
        ((to_px((x, y))[1], x, y, text, color) for x, y, text, color in items), key=lambda t: t[0]
    )
    last = None
    for py, x, y, text, color in placed:
        target = py if last is None else max(py, last + min_gap_px)
        last = target
        ax.annotate(
            text,
            (x, y),
            xytext=(10, target - py),
            textcoords="offset pixels",
            va="center",
            fontsize=fontsize,
            color=INK_2,
        )
        ax.plot([x], [y], marker="o", markersize=4, color=color, zorder=4)


def latency_curve(memlat: dict[str, Any], out: Path) -> Path:
    """ns per load vs buffer size for the random/4k curve (or the first),
    with sysfs cache sizes as vertical lines and detected boundaries marked."""
    r = memlat["results"]
    curve = next((c for c in r["curves"] if c["name"] == "random/4k"), r["curves"][0])
    pts = curve["points"]
    xs = [p["size_bytes"] for p in pts]
    fig, ax = _figure()
    color = CURVE_COLORS.get(curve["name"], SERIES[0])
    ax.fill_between(
        xs,
        [p["min_ns"] for p in pts],
        [p["max_ns"] for p in pts],
        color=color,
        alpha=0.15,
        linewidth=0,
        label="min-max of repetitions",
    )
    ax.plot(xs, [p["median_ns"] for p in pts], color=color, linewidth=2, label=f"{curve['name']} median")
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    seen = set()
    for c in r["sysfs_caches"]:
        if c["type"] == "Instruction" or c["size_bytes"] in seen:
            continue
        seen.add(c["size_bytes"])
        ax.axvline(c["size_bytes"], color=MUTED, linewidth=0.8, zorder=1)
        # Labelled in the top margin, so a label never sits on the curve.
        ax.annotate(
            f"L{c['level']} {size_label(c['size_bytes'])}",
            (c["size_bytes"], 1.0),
            xycoords=("data", "axes fraction"),
            xytext=(0, 3),
            textcoords="offset points",
            fontsize=8,
            color=INK_2,
            ha="center",
            va="bottom",
        )
    by_size = {p["size_bytes"]: p["median_ns"] for p in pts}
    bx = [b["size_before"] for b in curve["boundaries"] if b["size_before"] in by_size]
    if bx:
        ax.scatter(
            bx,
            [by_size[x] for x in bx],
            s=48,
            facecolor=SURFACE,
            edgecolor=INK,
            linewidth=1.2,
            zorder=5,
            label="detected boundary (last size before the jump)",
        )
    _size_axis(ax, xs)
    _style(ax, f"Memory latency, {curve['name']}", "buffer size (log2)", "ns per load (log)")
    ax.set_title(f"Memory latency, {curve['name']}", loc="left", color=INK, fontsize=12, pad=22)
    ax.legend(frameon=False, fontsize=8, loc="upper left", labelcolor=INK_2)
    fig.tight_layout()
    fig.savefig(out, facecolor=SURFACE)
    plt.close(fig)
    return out


def latency_modes(memlat: dict[str, Any], out: Path) -> Path | None:
    """random/4k vs random/huge vs sequential/4k on one chart."""
    curves = memlat["results"]["curves"]
    if len(curves) < 2:
        return None
    fig, ax = _figure()
    sizes: list[int] = []
    labels = []
    for c in curves:
        pts = c["points"]
        xs = [p["size_bytes"] for p in pts]
        ys = [p["median_ns"] for p in pts]
        sizes = sorted(set(sizes) | set(xs))
        color = CURVE_COLORS.get(c["name"], SERIES[len(sizes) % len(SERIES)])
        ax.plot(xs, ys, color=color, linewidth=2, label=c["name"])
        labels.append((xs[-1], ys[-1], f"{c['name']} {ys[-1]:.1f} ns", color))
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    _size_axis(ax, sizes)
    ax.set_xlim(right=sizes[-1] * 4)  # room for the end labels
    _style(ax, "Latency by access pattern and page size", "buffer size (log2)", "ns per load (log)")
    _end_labels(ax, fig, labels)
    ax.legend(frameon=False, fontsize=8, loc="upper left", labelcolor=INK_2)
    fig.tight_layout()
    fig.savefig(out, facecolor=SURFACE)
    plt.close(fig)
    return out


def bandwidth(membw: dict[str, Any], out: Path) -> Path | None:
    """Best GB/s per kernel vs number of threads."""
    r = membw["results"]
    if r.get("mode") != "sweep":
        return None
    runs = r["runs"]
    threads = [run["threads"] for run in runs]
    fig, ax = _figure()
    labels = []
    for kernel, color in KERNEL_COLORS.items():
        ys = [run["kernels"][kernel]["best_gbs"] for run in runs]
        ax.plot(threads, ys, color=color, linewidth=2, marker="o", markersize=4, label=kernel)
        labels.append((threads[-1], ys[-1], f"{kernel} {ys[-1]:.1f}", color))
    ax.set_xticks(threads)
    ax.set_xlim(0.5, threads[-1] + max(1.0, threads[-1] * 0.25))
    ax.set_ylim(bottom=0)
    _style(ax, "Memory bandwidth (best of repetitions)", "threads (one per core, pinned)", "GB/s")
    _end_labels(ax, fig, labels)
    ax.legend(frameon=False, fontsize=8, loc="upper left", labelcolor=INK_2)
    fig.tight_layout()
    fig.savefig(out, facecolor=SURFACE)
    plt.close(fig)
    return out


def numa(node_docs: dict[int, dict[str, Any]], local_node: int, out: Path) -> Path | None:
    """Latency at each size with memory on each node; the CPU stays on local_node."""
    if len(node_docs) < 2:
        return None
    fig, ax = _figure(8, 4.2)
    nodes = sorted(node_docs)
    sizes = [p["size_bytes"] for p in node_docs[nodes[0]]["results"]["curves"][0]["points"]]
    width = 0.8 / len(nodes)
    for i, node in enumerate(nodes):
        pts = {p["size_bytes"]: p["median_ns"] for p in node_docs[node]["results"]["curves"][0]["points"]}
        xs = [j + (i - (len(nodes) - 1) / 2) * width for j in range(len(sizes))]
        label = f"memory on node {node}" + (" (local)" if node == local_node else " (remote)")
        ax.bar(
            xs, [pts.get(s, 0) for s in sizes], width=width * 0.92, color=SERIES[i % len(SERIES)], label=label
        )
    ax.set_xticks(range(len(sizes)), [size_label(s) for s in sizes])
    _style(ax, f"NUMA: local vs remote latency (CPU on node {local_node})", "buffer size", "ns per load")
    ax.legend(frameon=False, fontsize=8, labelcolor=INK_2)
    fig.tight_layout()
    fig.savefig(out, facecolor=SURFACE)
    plt.close(fig)
    return out


def stress(samples: list[dict[str, Any]], throttled: list[tuple[float, float]], out: Path) -> Path:
    """Frequency, temperature and bandwidth over time as three stacked
    charts sharing the time axis (never two y-scales on one chart)."""
    t = [s["t"] for s in samples]
    rows = [
        ("CPU frequency (mean of all CPUs)", "MHz", [s["freq_mhz_mean"] for s in samples], True),
        ("Hottest temperature sensor", "C", [s["temp_c_max"] for s in samples], False),
        ("Triad bandwidth, all threads", "GB/s", [s["gbs"] for s in samples], True),
    ]
    fig, axes = plt.subplots(3, 1, figsize=(9, 7.5), dpi=130, sharex=True)
    fig.patch.set_facecolor(SURFACE)
    for ax, (title, unit, ys, from_zero) in zip(axes, rows, strict=True):
        pairs = [(x, y) for x, y in zip(t, ys, strict=True) if y is not None]
        _style(ax, title, "", unit)
        if pairs:
            ax.plot([p[0] for p in pairs], [p[1] for p in pairs], color=SERIES[0], linewidth=2)
            if from_zero:
                ax.set_ylim(bottom=0, top=max(p[1] for p in pairs) * 1.1)
        else:
            ax.text(
                0.5,
                0.5,
                "not available on this machine",
                transform=ax.transAxes,
                ha="center",
                va="center",
                color=MUTED,
                fontsize=9,
            )
            ax.set_yticks([])
        for start, end in throttled:
            ax.axvspan(start, end, color=STATUS_SERIOUS, alpha=0.18, linewidth=0)
    if t:
        axes[-1].set_xlim(0, max(t))
    axes[-1].set_xlabel("seconds", color=INK_2, fontsize=9)
    if throttled:
        axes[0].annotate(
            "shaded: thermal throttling detected",
            (0.99, 0.95),
            xycoords="axes fraction",
            ha="right",
            va="top",
            fontsize=8,
            color=INK_2,
        )
    fig.tight_layout()
    fig.savefig(out, facecolor=SURFACE)
    plt.close(fig)
    return out
