"""The shared result format: loading, validating and writing result files."""

from __future__ import annotations

import json
import os
import platform
import socket
from datetime import datetime, timezone
from functools import lru_cache
from importlib import resources
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator

FORMAT_VERSION = 1


class ResultError(ValueError):
    """A result file is missing, unreadable or doesn't match the schema."""


@lru_cache(maxsize=1)
def schema() -> dict[str, Any]:
    return json.loads(resources.files("hwdiag").joinpath("schema.json").read_text())


@lru_cache(maxsize=1)
def _validator() -> Draft202012Validator:
    return Draft202012Validator(schema())


def validation_errors(doc: Any) -> list[str]:
    """Every schema violation in doc, as 'path: message' strings."""
    errors = sorted(_validator().iter_errors(doc), key=lambda e: list(e.absolute_path))
    out = []
    for e in errors:
        path = "/".join(str(p) for p in e.absolute_path) or "(root)"
        out.append(f"{path}: {e.message}")
    return out


def validate(doc: Any, source: str = "result") -> None:
    errors = validation_errors(doc)
    if errors:
        shown = "\n  ".join(errors[:10])
        more = f"\n  ... and {len(errors) - 10} more" if len(errors) > 10 else ""
        raise ResultError(f"{source} does not match the hwdiag schema:\n  {shown}{more}")


def load(path: str | os.PathLike[str]) -> dict[str, Any]:
    """Reads and validates one result file."""
    p = Path(path)
    try:
        doc = json.loads(p.read_text())
    except OSError as e:
        raise ResultError(f"cannot read {p}: {e}") from e
    except json.JSONDecodeError as e:
        raise ResultError(f"{p} is not valid JSON: {e}") from e
    validate(doc, str(p))
    return doc


def save(doc: dict[str, Any], path: str | os.PathLike[str]) -> Path:
    """Validates and writes a result file."""
    validate(doc, str(path))
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(doc, indent=2) + "\n")
    return p


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def python_host() -> dict[str, Any]:
    """A host block for results the harness writes itself, when no tool
    result is at hand to copy one from."""

    def read(path: str) -> str | None:
        try:
            return Path(path).read_text().strip() or None
        except OSError:
            return None

    model = "unknown"
    cpuinfo = read("/proc/cpuinfo") or ""
    for line in cpuinfo.splitlines():
        if line.startswith("model name"):
            model = line.split(":", 1)[1].strip()
            break
    no_turbo = read("/sys/devices/system/cpu/intel_pstate/no_turbo")
    boost = read("/sys/devices/system/cpu/cpufreq/boost")
    turbo = "unknown"
    if no_turbo is not None:
        turbo = "on" if no_turbo == "0" else "off"
    elif boost is not None:
        turbo = "on" if boost != "0" else "off"
    smt_active = read("/sys/devices/system/cpu/smt/active")
    return {
        "hostname": socket.gethostname(),
        "kernel": platform.release(),
        "cpu_model": model,
        "logical_cpus": os.cpu_count() or 1,
        "governor": read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "turbo": turbo,
        "smt": "unknown" if smt_active is None else ("on" if smt_active != "0" else "off"),
        "compiler": f"python {platform.python_version()}",
        "compiler_flags": "",
        "build_type": "",
    }


def make_result(
    tool: str,
    params: dict[str, Any],
    results: dict[str, Any],
    warnings: list[str] | None = None,
    host: dict[str, Any] | None = None,
    git_commit: str = "unknown",
    tool_version: str | None = None,
) -> dict[str, Any]:
    from . import __version__

    return {
        "format_version": FORMAT_VERSION,
        "tool": tool,
        "tool_version": tool_version or __version__,
        "timestamp": utc_now(),
        "git_commit": git_commit,
        "host": host or python_host(),
        "params": params,
        "results": results,
        "warnings": list(warnings or []),
    }
