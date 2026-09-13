# Validation record — September 13, 2026

Recorded timings are unchanged. Project identifiers and header paths in the
saved source snapshots were normalized after measurement, and absolute build
paths are represented as `<project>`. Their manifests identify this adjustment
and hash the normalized sources. The [original records](https://github.com/BahadirAydin/false-sharing/tree/23dca052bc3344211d1d3d22b8aa26c4a9ca91b0/results)
remain available in Git history, including the original source archives.

## Correctness and build checks

| Configuration | Outcome |
| --- | --- |
| Clang 22.1.8, C++23, Release | All 24 test groups passed |
| GCC 16.2.1, C++23, Release | All 24 test groups passed |
| Clang 22.1.8, Debug, ThreadSanitizer | All 24 test groups passed, no reported races |
| Clang 22.1.8, Debug, AddressSanitizer + UBSan | All 24 test groups passed with leak detection disabled |
| clang-format dry-run | Passed |
| Python script byte compilation | Passed |

LeakSanitizer initially refused to execute under the traced sandbox. The
address/UB sanitizer run was repeated with `ASAN_OPTIONS=detect_leaks=0`.
This records no claim of a successful leak-sanitizer run. Lifetime tests
explicitly check destruction, including destruction with queued values, but
are not a general leak detector.

The test suite exercises randomized operations against a deque, capacity-one
boundaries, repeated wraparound, failed-push ownership preservation, move-only
payloads, destruction, and concurrent per-item FIFO validation at three
capacities. Controlled gates pause construction and consumer movement to
exercise publication and reclamation behavior. Those gates add synchronization;
they do not establish correctness for every weak-memory execution.

GCC's benchmark disassembly was inspected and contains locked atomic-add
instructions in the counter workers. No per-instruction timing claim or
hardware-event attribution is inferred from that inspection.

## Performance results

Both runs used the same implementation and GCC build, 64-byte configured and
detected line size, CPUs 1 and 2 on different physical cores, and CPU 5 as
CPU 1's SMT sibling. Each case transferred/incremented 5,000,000 items per
measured round, across 15 rounds. Seed changed from 20260913 to 20260914.
All timed queue transfers passed per-item FIFO validation.

| Median comparison | First run | Repeat |
| --- | ---: | ---: |
| Shared-line counters, ns/increment per thread | 24.263 | 35.353 |
| Separated counters, ns/increment per thread | 4.224 | 4.220 |
| Shared / separated counter time ratio | 5.74 | 8.38 |
| Shared uncached queue, amortized ns/item | 15.895 | 18.842 |
| Shared cached queue, amortized ns/item | 19.214 | 21.244 |
| Separated uncached queue, amortized ns/item | 9.554 | 17.741 |
| Separated cached queue, amortized ns/item | 7.314 | 5.111 |
| Separated cached / shared uncached throughput ratio | 2.17 | 3.69 |

- [First run: full summary and spread](../results/2026-09-13-gcc16-i7-10510u/summary.md)
- [Repeat: full summary and spread](../results/2026-09-13-gcc16-i7-10510u-repeat/summary.md)

Separating the counters gave a substantially lower median time in both runs.
For this queue, the separated/cached variant had the best median in both runs,
while caching with shared indices was slower than the shared/uncached baseline.
That is a useful result: the two changes interact, and caching is not a
standalone promise of improvement.

Absolute times and several effect sizes changed substantially between runs.
The saved min/max ranges and the two independent sets of medians should remain
visible in any article. Do not select the faster run as the only result.

The laptop used the powersave governor with turbo enabled and was not isolated
from background work. Frequency snapshots changed over the first run. These
observations do not isolate the cause of cross-run differences; scheduling,
frequency/thermal behavior, and placement effects need controlled follow-up
before attributing a cause. More repetitions alone would not establish one.

The optional uint64 slot occupies 16 bytes in this build. The comparisons
therefore apply to this ownership representation, including engagement-flag
accesses and per-item validation. They do not establish the same speedup for a
raw-storage queue, a different payload/capacity, or a different machine.

No message latency distribution was measured. No cycle counts or exact
coherence-traffic costs are reported. The existing website article was not
changed or published as part of this project task.
