# hwdiag

Linux hardware diagnostics: a sysfs inventory with health checks, memory latency and bandwidth
benchmarks, hardware performance counters and a user-space memory test, with a Python harness
for reports and regression checks.

Work in progress.

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
./build/bin/inventory
```
