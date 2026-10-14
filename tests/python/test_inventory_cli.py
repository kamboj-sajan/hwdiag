from __future__ import annotations

import argparse
import shutil

from conftest import FIXTURES, load_json
from hwdiag import inventory


def test_summary_of_the_desktop_fixture_shows_the_problems():
    text = inventory.summarize(load_json(FIXTURES / "results" / "inventory_desktop.json"))
    assert "Intel(R) Core(TM) i7-7700K" in text
    assert "1 socket(s), 4 core(s), 8 thread(s), SMT on" in text
    assert "L1d 32K, L1i 32K, L2 256K, L3 8M (line 64 B)" in text
    assert "0000:01:00.0 VGA controller: 2.5 GT/s x16 (max 16 GT/s x16)" in text
    assert "[warn] pcie_link 0000:01:00.0" in text
    assert "re-check under load" in text
    assert "hottest coretemp/Package id 0 91.0 C" in text
    assert "EDAC       not present" in text
    assert text.endswith("Status     warn")


def test_summary_of_the_server_fixture_fails_on_edac():
    text = inventory.summarize(load_json(FIXTURES / "results" / "inventory_server.json"))
    assert "2 socket(s)" in text
    assert "2 NUMA node(s)" in text
    assert "3 corrected, 0 uncorrected" in text
    assert "cpufreq not available" in text
    assert text.endswith("Status     fail")


def test_human_bytes():
    assert inventory.human_bytes(48 * 1024) == "48K"
    assert inventory.human_bytes(260 * 1024 * 1024) == "260M"
    assert inventory.human_bytes(1536) == "1.5K"


def test_command_exit_code_follows_the_status(tmp_path, fake_bin, monkeypatch, capsys):
    src = tmp_path / "src"
    src.mkdir()
    shutil.copy(FIXTURES / "results" / "inventory_server.json", src / "inventory.json")
    monkeypatch.setenv("FAKE_SRC", str(src))
    monkeypatch.setenv("FAKE_EXIT_INVENTORY", "2")
    args = argparse.Namespace(root="/", temp_threshold=85.0, json=False, out=None, bin_dir=str(fake_bin))
    assert inventory.command(args) == 2
    assert "Status     fail" in capsys.readouterr().out
    shutil.copy(FIXTURES / "results" / "inventory_desktop.json", src / "inventory.json")
    monkeypatch.setenv("FAKE_EXIT_INVENTORY", "0")
    assert inventory.command(args) == 0
