#!/usr/bin/env python3
"""Generate the fake /proc and /sys trees under tests/fixtures/sys/.

desktop: 4 cores / 8 threads with SMT, a GPU whose link has dropped to
         2.5 GT/s x16 (max 16 GT/s x16), one sensor at 91 C, no EDAC.
server:  2 sockets x 4 cores, SMT off, 2 NUMA nodes, no cpufreq,
         a NIC running x8 in an x16 slot, EDAC with ce_count 3 on mc0.

Run from the repo root: python3 tests/fixtures/make_sys_fixtures.py
"""

from __future__ import annotations

import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent / "sys"


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel.lstrip("/")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text if text.endswith("\n") else text + "\n")


def cpuinfo(model: str, vendor: str, n: int, mhz: float, flags: str) -> str:
    blocks = []
    for i in range(n):
        blocks.append(
            f"processor\t: {i}\nvendor_id\t: {vendor}\ncpu family\t: 6\n"
            f"model name\t: {model}\ncpu MHz\t\t: {mhz:.3f}\nflags\t\t: {flags}\n"
        )
    return "\n".join(blocks)


def caches(root: Path, cpu: int, levels: list[tuple[int, str, str, str]]) -> None:
    for idx, (level, typ, size, shared) in enumerate(levels):
        base = f"/sys/devices/system/cpu/cpu{cpu}/cache/index{idx}/"
        write(root, base + "level", str(level))
        write(root, base + "type", typ)
        write(root, base + "size", size)
        write(root, base + "coherency_line_size", "64")
        write(root, base + "shared_cpu_list", shared)


def pci(root: Path, addr: str, vendor: str, device: str, cls: str, link: tuple | None) -> None:
    base = f"/sys/bus/pci/devices/{addr}/"
    write(root, base + "vendor", vendor)
    write(root, base + "device", device)
    write(root, base + "class", cls)
    if link:
        cur_speed, cur_width, max_speed, max_width = link
        write(root, base + "current_link_speed", cur_speed)
        write(root, base + "current_link_width", str(cur_width))
        write(root, base + "max_link_speed", max_speed)
        write(root, base + "max_link_width", str(max_width))


def desktop(root: Path) -> None:
    flags = "fpu vme de pse tsc msr pae mce cx8 apic sep mtrr sse sse2 ht avx avx2 fma bmi2"
    write(root, "/proc/cpuinfo", cpuinfo("Intel(R) Core(TM) i7-7700K CPU @ 4.20GHz", "GenuineIntel", 8, 4200.0, flags))
    write(
        root,
        "/proc/meminfo",
        "MemTotal:       32768000 kB\nMemFree:        20000000 kB\nMemAvailable:   28000000 kB\n"
        "HugePages_Total:       0\nHugePages_Free:        0\nHugepagesize:       2048 kB\n",
    )
    for cpu in range(8):
        core = cpu % 4  # Intel numbering: cpu n and n+4 are SMT siblings
        base = f"/sys/devices/system/cpu/cpu{cpu}/"
        write(root, base + "topology/physical_package_id", "0")
        write(root, base + "topology/die_id", "0")
        write(root, base + "topology/core_id", str(core))
        write(root, base + "topology/thread_siblings_list", f"{core},{core + 4}")
        write(root, base + "cpufreq/scaling_cur_freq", str(4400000 - cpu * 100000))
        write(root, base + "cpufreq/cpuinfo_min_freq", "800000")
        write(root, base + "cpufreq/cpuinfo_max_freq", "4500000")
        write(root, base + "cpufreq/scaling_governor", "powersave")
    caches(root, 0, [(1, "Data", "32K", "0,4"), (1, "Instruction", "32K", "0,4"), (2, "Unified", "256K", "0,4"), (3, "Unified", "8192K", "0-7")])
    write(root, "/sys/devices/system/cpu/intel_pstate/no_turbo", "0")
    write(root, "/sys/devices/system/cpu/smt/active", "1")
    write(root, "/sys/devices/system/node/node0/cpulist", "0-7")
    write(root, "/sys/devices/system/node/node0/meminfo", "Node 0 MemTotal:       32768000 kB\nNode 0 MemFree:        20000000 kB\n")
    pci(root, "0000:00:00.0", "0x8086", "0x591f", "0x060000", None)
    pci(root, "0000:01:00.0", "0x10de", "0x2484", "0x030000", ("2.5 GT/s PCIe", 16, "16.0 GT/s PCIe", 16))
    pci(root, "0000:02:00.0", "0x144d", "0xa808", "0x010802", ("8.0 GT/s PCIe", 4, "8.0 GT/s PCIe", 4))
    pci(root, "0000:03:00.0", "0x8086", "0x15b8", "0x020000", ("Unknown", 0, "2.5 GT/s PCIe", 1))
    write(root, "/sys/class/hwmon/hwmon0/name", "coretemp")
    write(root, "/sys/class/hwmon/hwmon0/temp1_input", "91000")
    write(root, "/sys/class/hwmon/hwmon0/temp1_label", "Package id 0")
    write(root, "/sys/class/hwmon/hwmon0/temp2_input", "55000")
    write(root, "/sys/class/hwmon/hwmon0/temp2_label", "Core 0")
    write(root, "/sys/class/hwmon/hwmon1/name", "nvme")
    write(root, "/sys/class/hwmon/hwmon1/temp1_input", "42850")
    write(root, "/sys/class/hwmon/hwmon1/temp1_label", "Composite")
    write(root, "/sys/class/hwmon/hwmon2/name", "nct6775")
    write(root, "/sys/class/hwmon/hwmon2/in0_input", "1032")


def server(root: Path) -> None:
    flags = "fpu vme de pse tsc msr pae mce cx8 apic sep mtrr sse sse2 avx avx2 avx512f"
    write(root, "/proc/cpuinfo", cpuinfo("Intel(R) Xeon(R) Gold 6338 CPU @ 2.00GHz", "GenuineIntel", 8, 2000.0, flags))
    write(
        root,
        "/proc/meminfo",
        "MemTotal:       263921592 kB\nMemFree:        250000000 kB\nMemAvailable:   255000000 kB\n"
        "HugePages_Total:      16\nHugePages_Free:       16\nHugepagesize:       2048 kB\n",
    )
    for cpu in range(8):
        pkg, core = divmod(cpu, 4)
        base = f"/sys/devices/system/cpu/cpu{cpu}/topology/"
        write(root, base + "physical_package_id", str(pkg))
        write(root, base + "die_id", "0")
        write(root, base + "core_id", str(core))
        write(root, base + "thread_siblings_list", str(cpu))
    caches(root, 0, [(1, "Data", "48K", "0"), (1, "Instruction", "32K", "0"), (2, "Unified", "1280K", "0"), (3, "Unified", "49152K", "0-3")])
    write(root, "/sys/devices/system/cpu/smt/active", "0")
    for node, cpus in ((0, "0-3"), (1, "4-7")):
        write(root, f"/sys/devices/system/node/node{node}/cpulist", cpus)
        write(root, f"/sys/devices/system/node/node{node}/meminfo", f"Node {node} MemTotal:       131960796 kB\n")
    pci(root, "0000:00:00.0", "0x8086", "0x09a2", "0x088000", None)
    pci(root, "0000:17:00.0", "0x15b3", "0x101d", "0x020000", ("8.0 GT/s PCIe", 8, "8.0 GT/s PCIe", 16))
    pci(root, "0000:b1:00.0", "0x144d", "0xa824", "0x010802", ("16.0 GT/s PCIe", 4, "16.0 GT/s PCIe", 4))
    write(root, "/sys/class/hwmon/hwmon0/name", "coretemp")
    write(root, "/sys/class/hwmon/hwmon0/temp1_input", "60000")
    write(root, "/sys/class/hwmon/hwmon0/temp1_label", "Package id 0")
    write(root, "/sys/devices/system/edac/mc/mc0/ce_count", "3")
    write(root, "/sys/devices/system/edac/mc/mc0/ue_count", "0")
    write(root, "/sys/devices/system/edac/mc/mc1/ce_count", "0")
    write(root, "/sys/devices/system/edac/mc/mc1/ue_count", "0")


def main() -> None:
    for name, build in (("desktop", desktop), ("server", server)):
        root = HERE / name
        if root.exists():
            shutil.rmtree(root)
        build(root)
        print(f"wrote {root}")


if __name__ == "__main__":
    main()
