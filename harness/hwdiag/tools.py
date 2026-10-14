"""Finding and running the C++ tools."""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

TOOLS = ("inventory", "memlat", "membw", "memtest", "perfcheck")


class ToolNotFound(FileNotFoundError):
    pass


def repo_root() -> Path:
    """The repository root when running from a source checkout
    (harness/hwdiag/tools.py -> repo)."""
    return Path(__file__).resolve().parents[2]


def find_tool(name: str, bin_dir: str | os.PathLike[str] | None = None) -> Path:
    """Looks for a tool in --bin-dir, then $HWDIAG_BIN_DIR, then
    <repo>/build/bin, then PATH."""
    candidates: list[Path] = []
    if bin_dir:
        candidates.append(Path(bin_dir))
    if os.environ.get("HWDIAG_BIN_DIR"):
        candidates.append(Path(os.environ["HWDIAG_BIN_DIR"]))
    candidates.append(repo_root() / "build" / "bin")
    for d in candidates:
        p = d / name
        if p.is_file() and os.access(p, os.X_OK):
            return p
    found = shutil.which(name)
    if found:
        return Path(found)
    searched = ", ".join(str(d) for d in candidates)
    raise ToolNotFound(
        f"cannot find the '{name}' executable (looked in {searched} and PATH). "
        "Build it first: cmake -S . -B build && cmake --build build"
    )


def git_commit() -> str | None:
    """Short commit of the source checkout, or None outside a git repo."""
    try:
        out = subprocess.run(
            ["git", "-C", str(repo_root()), "rev-parse", "--short=12", "HEAD"],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    commit = out.stdout.strip()
    return commit if out.returncode == 0 and commit else None


@dataclass
class ToolRun:
    tool: str
    exit_code: int
    seconds: float
    out: Path | None  # the result file, when the tool wrote one
    command: list[str]


def run_tool(
    name: str,
    args: list[str],
    out: Path,
    bin_dir: str | os.PathLike[str] | None = None,
    env: dict[str, str] | None = None,
    wrapper: list[str] | None = None,
) -> ToolRun:
    """Runs one tool with --out <out>. stderr (progress lines) goes to the
    terminal. A missing executable counts as exit code 127."""
    try:
        exe = find_tool(name, bin_dir)
    except ToolNotFound as e:
        print(f"hwdiag: {e}")
        return ToolRun(name, 127, 0.0, None, [name, *args])
    cmd = [*(wrapper or []), str(exe), *args, "--out", str(out)]
    full_env = {**os.environ, **(env or {})}
    t0 = time.monotonic()
    proc = subprocess.run(cmd, env=full_env, check=False)
    seconds = time.monotonic() - t0
    return ToolRun(name, proc.returncode, seconds, out if out.exists() else None, cmd)
