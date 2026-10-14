from __future__ import annotations

import pytest
from conftest import FIXTURES, SAMPLE_RUN, load_json
from hwdiag import results


@pytest.mark.parametrize("path", sorted(SAMPLE_RUN.glob("*.json")), ids=lambda p: p.name)
def test_sample_run_files_match_the_schema(path):
    results.load(path)


@pytest.mark.parametrize("name", ["inventory_desktop.json", "inventory_server.json"])
def test_fixture_inventories_match_the_schema(name):
    doc = results.load(FIXTURES / "results" / name)
    assert doc["tool"] == "inventory"


def test_schema_is_draft_2020_12():
    assert results.schema()["$schema"] == "https://json-schema.org/draft/2020-12/schema"


def broken(doc, path, value=..., delete=False):
    target = doc
    for key in path[:-1]:
        target = target[key]
    if delete:
        del target[path[-1]]
    else:
        target[path[-1]] = value
    return doc


@pytest.mark.parametrize(
    "tool, path, value, delete, fragment",
    [
        ("memlat", ["host"], None, True, "'host' is a required property"),
        ("memlat", ["format_version"], 2, False, "format_version"),
        ("memlat", ["tool"], "memcheck", False, "tool"),
        ("memlat", ["timestamp"], "yesterday", False, "timestamp"),
        ("memlat", ["host", "turbo"], "maybe", False, "host/turbo"),
        ("memlat", ["results", "curves"], [], False, "results/curves"),
        ("memlat", ["results", "curves", 0, "points", 0, "median_ns"], -1, False, "median_ns"),
        ("membw", ["results", "runs", 0, "kernels", "triad"], None, True, "'triad' is a required property"),
        ("membw", ["results", "mode"], "burst", False, "results/mode"),
        ("memtest", ["results", "status"], "ok", False, "results/status"),
        ("inventory", ["results", "checks", 0, "level"], "error", False, "level"),
        ("inventory", ["results", "status"], None, True, "'status' is a required property"),
        ("perfcheck", ["results", "counters"], {"available": True, "reason": None}, False, "counters"),
        ("run", ["results", "suite"], "medium", False, "results/suite"),
    ],
)
def test_broken_results_are_rejected(sample_doc, tool, path, value, delete, fragment):
    doc = broken(sample_doc(tool), path, value, delete)
    errors = results.validation_errors(doc)
    assert errors, "the broken document validated"
    assert any(fragment in e for e in errors), errors


def test_extra_top_level_keys_are_rejected(sample_doc):
    doc = sample_doc("memtest")
    doc["notes"] = "hello"
    with pytest.raises(results.ResultError, match="Additional properties"):
        results.validate(doc)


def test_memtest_error_masks_must_be_64_bit_hex(sample_doc):
    doc = sample_doc("memtest")
    doc["results"]["errors"] = [
        {
            "pattern": "zeros",
            "offset": 8,
            "expected": "0x0",
            "actual": "0x1",
            "xor_mask": "0x1",
            "flipped_bits": [0],
        }
    ]
    assert any("expected" in e for e in results.validation_errors(doc))
    doc["results"]["errors"][0].update(
        expected="0x0000000000000000", actual="0x0000000000000001", xor_mask="0x0000000000000001"
    )
    assert results.validation_errors(doc) == []


def test_load_reports_unreadable_and_invalid_files(tmp_path):
    with pytest.raises(results.ResultError, match="cannot read"):
        results.load(tmp_path / "missing.json")
    bad = tmp_path / "bad.json"
    bad.write_text("{not json")
    with pytest.raises(results.ResultError, match="not valid JSON"):
        results.load(bad)


def test_make_result_with_python_host_is_valid():
    doc = results.make_result(
        "stress",
        {"seconds": 1},
        {
            "samples": [],
            "throttling": {"detected": False, "reasons": [], "baseline_mhz": None},
            "summary": {},
        },
    )
    results.validate(doc)
    assert doc["host"]["logical_cpus"] >= 1


def test_save_refuses_invalid_documents(tmp_path, sample_doc):
    doc = sample_doc("membw")
    del doc["results"]["peak"]
    with pytest.raises(results.ResultError):
        results.save(doc, tmp_path / "x.json")
    assert not (tmp_path / "x.json").exists()


def test_every_sample_was_measured_on_one_host():
    hosts = {load_json(p)["host"]["cpu_model"] for p in SAMPLE_RUN.glob("*.json")}
    assert len(hosts) == 1
