from __future__ import annotations

import copy
import json
import os
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "tests" / "fixtures"
SAMPLE_RUN = FIXTURES / "results" / "sample_run"
SYS = FIXTURES / "sys"


def load_json(path: Path) -> dict:
    return json.loads(path.read_text())


@pytest.fixture
def sample_run() -> Path:
    return SAMPLE_RUN


@pytest.fixture
def sample_doc():
    """A deep copy of one sample result, by tool name, safe to modify."""

    def get(tool: str) -> dict:
        return copy.deepcopy(load_json(SAMPLE_RUN / f"{tool}.json"))

    return get


FAKE_TOOL = """#!{python}
import json, os, shutil, sys
name = os.path.basename(sys.argv[0])
args = sys.argv[1:]
out = args[args.index("--out") + 1]
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(json.dumps([name] + args) + "\\n")
code = int(os.environ.get("FAKE_EXIT_" + name.upper(), "0"))
if code != 1:
    shutil.copy(os.path.join(os.environ["FAKE_SRC"], name + ".json"), out)
sys.exit(code)
"""


class FakeBin(os.PathLike):
    """The fake bin directory; also lists the calls the fake tools logged."""

    def __init__(self, path: Path, log: Path):
        self.path = path
        self.log = log

    def __fspath__(self) -> str:
        return str(self.path)

    def __str__(self) -> str:
        return str(self.path)

    def __truediv__(self, name: str) -> Path:
        return self.path / name

    def calls(self) -> list[list[str]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]


@pytest.fixture
def fake_bin(tmp_path, monkeypatch):
    """A bin directory of scripts standing in for the C++ tools: each copies
    the sample result for its name to --out and logs its arguments."""
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    for name in ("inventory", "memlat", "membw", "memtest", "perfcheck"):
        p = bin_dir / name
        p.write_text(FAKE_TOOL.format(python=sys.executable))
        p.chmod(0o755)
    log = tmp_path / "calls.log"
    monkeypatch.setenv("FAKE_LOG", str(log))
    monkeypatch.setenv("FAKE_SRC", str(SAMPLE_RUN))
    monkeypatch.delenv("HWDIAG_BIN_DIR", raising=False)
    return FakeBin(bin_dir, log)
