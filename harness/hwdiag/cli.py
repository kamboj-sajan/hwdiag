"""Command line: hwdiag inventory | run | report | compare | stress."""

from __future__ import annotations

import argparse
import sys

from . import __version__, results


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="hwdiag", description="Linux hardware diagnostics harness.")
    p.add_argument("--version", action="version", version=f"hwdiag {__version__}")
    p.add_argument(
        "--bin-dir", help="directory with the C++ tools (default: $HWDIAG_BIN_DIR, build/bin, PATH)"
    )
    sub = p.add_subparsers(dest="command", required=True)

    inv = sub.add_parser("inventory", help="show the hardware inventory and health checks")
    inv.add_argument("--root", default="/", help="filesystem root to read /proc and /sys from")
    inv.add_argument("--temp-threshold", type=float, default=85.0, help="warn above this temperature (C)")
    inv.add_argument("--json", action="store_true", help="print the JSON result instead of a summary")
    inv.add_argument("--out", help="also keep the JSON result in this file")

    run = sub.add_parser("run", help="run a suite of tools into a run directory")
    run.add_argument("--suite", choices=["quick", "full"], default="quick")
    run.add_argument("--out", help="run directory (default: results/<host>-<UTC time>)")

    rep = sub.add_parser("report", help="plots and a Markdown report for a run directory")
    rep.add_argument("run", help="run directory (or its run.json)")
    rep.add_argument("--out", help="where to write REPORT.md and the plots (default: the run directory)")

    cmp_ = sub.add_parser("compare", help="compare two runs; exit 1 if anything got worse than the tolerance")
    cmp_.add_argument("base", help="baseline run directory or result file")
    cmp_.add_argument("new", help="new run directory or result file")
    cmp_.add_argument("--tolerance", type=float, default=5.0, help="allowed slowdown in percent")
    cmp_.add_argument("--json", action="store_true", help="print the comparison as JSON")

    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "inventory":
            from . import inventory

            return inventory.command(args)
        if args.command == "run":
            from . import run

            return run.command(args)
        if args.command == "report":
            from . import report

            return report.command(args)
        if args.command == "compare":
            from . import compare

            return compare.command(args)
    except results.ResultError as e:
        print(f"hwdiag: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 1


if __name__ == "__main__":
    sys.exit(main())
