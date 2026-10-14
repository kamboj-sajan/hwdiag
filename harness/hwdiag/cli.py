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
    except results.ResultError as e:
        print(f"hwdiag: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 1


if __name__ == "__main__":
    sys.exit(main())
