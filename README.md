# False sharing in two counters and an SPSC queue

C++23 experiments in concurrency and performance. The first experiment compares
false sharing in two atomic counters and in a single-producer/single-consumer
queue.

The question is specific: what changes when two threads write indices on the
same cache line, and how much does caching the remote index change the result?
There are four queue variants so those choices can be measured separately.

## Build and test

Requirements: Linux for the benchmark, a compiler and standard library with
C++23 support, CMake 3.25+, Ninja, and Python 3.10+. There are no downloaded or
vendored dependencies. The queue itself does not use Linux APIs.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build-release -j 2
ctest --test-dir build-release --output-on-failure
```

## Run the experiment

```bash
python3 scripts/run_experiment.py
```

The runner builds Release, runs the tests, selects two allowed physical cores
in one package and an SMT sibling when available, and checks the configured
cache-line size against sysfs. It fails on invalid affinity or layout instead
of silently running a different experiment.

Each case has an untimed warm-up followed by 11 measured rounds. Case order is
shuffled in each round with a recorded seed. By default each counter thread
performs two million increments and each queue transfers two million items.
The output is stored in a new directory under `results/`:

- `samples.csv`: every measured round, including full/empty queue retry counts.
- `environment.json`: CPU topology, compiler and flags, affinity, governor,
  frequency snapshots, workload, binary hash, and source hashes.
- `source.tar.gz`: the exact implementation, tests, and scripts for that run.
- `tests.txt` and `benchmark.log`: validation and run records.
- `summary.md` and `comparison.svg`: descriptive statistics and a chart.

To choose the placement and increase the workload:

```bash
python3 scripts/run_experiment.py --cpu-a 1 --cpu-b 2 --cpu-smt 5 \
  --items 5000000 --rounds 15
```

Those CPU IDs are an example for the machine used during development. Use your
machine's topology. The script checks that A/B are different physical cores
and SMT is A's sibling. Omit `--cpu-smt` to autodetect it. On a machine without
an allowed sibling, the SMT control is omitted.

Use `--compiler clang++ --build-dir build-clang` for a separate compiler build.
Use `--line-size N` only for the detected line size of your target. Each output
directory is unique; an explicit `--output PATH` must not already exist. Run
benchmarks while the machine is otherwise quiet. The runner does not change
power settings, disable turbo, or claim the CPU holds a fixed clock speed.

## Recorded results

Two 15-round runs on an i7-10510U are included. Separating counter lines reduced
median time by factors of 5.74 and 8.38. The separated/cached queue achieved
2.17 and 3.69 times the shared/uncached queue throughput. These are two observed
ratios, not a confidence interval or a universal speedup. See the
[validation record](docs/validation.md) for both runs, full spreads, sanitizer
results, and the substantial variation between runs.

## The queue

```cpp
#include "cpp_work/spsc_queue.hpp"
#include <memory>

cpp_work::SpscQueue<std::unique_ptr<int>, 1024> queue;
auto item = std::make_unique<int>(42);
if (!queue.try_push(std::move(item))) {
  // Full. item still owns the value; the caller chooses what to do next.
}
if (auto received = queue.try_pop()) {
  // Empty is represented by std::nullopt; otherwise received owns the item.
}
```

In actual concurrent use, exactly one thread calls `try_push`/`try_emplace` and
exactly one calls `try_pop`. The default has separated indices and caches the
remote position. `1024` storage slots provide **1023 usable entries**, reserving
one slot to distinguish full from empty. Full/empty return immediately; retry,
drop, backpressure, and shutdown policy belong to the caller.

Payloads can be move-only and need not have a default constructor or assignment
operator. Moves and destruction must be nonthrowing; each `try_emplace` call
also requires nonthrowing construction. A failed insertion does not move from
its argument. Destruction of the queue destroys remaining items, after both
threads have stopped. There is no concurrent `size()` or reset API.

The queue owns fixed inline storage and does not dynamically allocate. Payload
operations can allocate or block. The implementation requires lock-free index
atomics, but makes **no unconditional wait-free claim** for arbitrary payloads
or retry loops. See [the design](docs/spsc-design.md) for memory ordering and
lifetime reasoning.

`std::optional<T>` tracks each slot's lifetime. This intentionally favors
readability over raw-storage machinery. It adds an engagement flag and can
increase slot size: the benchmark records the actual size. All four queue
variants use the same payload representation. The result is not a performance
claim about an alternative queue with packed raw payload storage.

## What the experiment measures

| Case | What it controls |
| --- | --- |
| One counter, one writer | Uncontended atomic-increment baseline |
| Two counters, one line, different cores | False sharing |
| Two counters, separate lines, different cores | Independent cache lines |
| One counter, two writers | True sharing control |
| Two counters, one line, SMT siblings | Same physical core control, when available |
| Queue: shared indices, no index cache | Queue baseline |
| Queue: shared indices, index cache | Caching without separation |
| Queue: separated indices, no index cache | Separation without caching |
| Queue: separated indices, index cache | Both changes |

The counter metric is wall time until the last writer finishes, divided by
increments **per thread**. The queue metric is wall time until the consumer
finishes, divided by delivered items. Both include release from the start
barrier and worker scheduling. Thread creation, pinning, and joins are outside
the timed interval. The queue checks every item's expected sequence number
inside the timed loop and records retries; that work is part of the result.

Queue `ns/item` is an **amortized throughput measure**, not the latency of one
message. Min/max/median describe repeated batch timings, not message latency
percentiles. We report wall-clock units, not cycles inferred from advertised
CPU frequency. See [the experiment protocol](docs/experiment.md).

## Sanitizers

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DCPP_WORK_SANITIZER=address
cmake --build build-asan --target spsc_queue_test -j 2
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DCPP_WORK_SANITIZER=thread
cmake --build build-tsan --target spsc_queue_test -j 2
ctest --test-dir build-tsan --output-on-failure
```

The address configuration enables AddressSanitizer and UndefinedBehaviorSanitizer.
Under a debugger/ptrace sandbox, LeakSanitizer may refuse to run. In that case,
`ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure`
checks address/undefined behavior with leak checking explicitly disabled.
Never use sanitizer-instrumented binaries for the performance comparison.

The test executable runs six groups for each of the four queue variants:
model/boundary tests, move-only/lifetime tests, controlled publication/reuse
checks, and concurrent FIFO at three capacities (including one usable entry).
Checks remain enabled in Release. CTest and blocked test operations have timeouts.
Passing tests and sanitizers provide evidence; the memory-order argument is
still necessary.

## Files

```text
include/cpp_work/spsc_queue.hpp   queue and index-layout policies
benchmarks/false_sharing.cpp     counter and queue experiment
tests/spsc_queue_test.cpp       standalone tests (no framework dependency)
scripts/run_experiment.py       build, validate topology, run, preserve evidence
scripts/summarize.py            regenerate summary and SVG from saved samples
docs/                          design, methodology, and validation record
results/                       measured runs, including source snapshots
```
