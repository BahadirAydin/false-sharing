# Experiment results

Machine: Intel(R) Core(TM) i7-10510U CPU @ 1.80GHz. 15 measured rounds per case; 5,000,000 operations per round.

Counters: elapsed wall time until all writers finish / increments per thread. Queue: elapsed wall time until the consumer finishes / delivered items. Thread creation and joins are outside the measured interval. Barrier release and worker scheduling are included.

| Group | Variant | Placement | Min ns/op | Median ns/op | Max ns/op |
| --- | --- | --- | ---: | ---: | ---: |
| counters | same_counter | different_cores | 34.639 | 35.365 | 39.191 |
| counters | separate_lines | different_cores | 4.211 | 4.220 | 4.812 |
| counters | shared_line | different_cores | 34.922 | 35.353 | 39.433 |
| counters | shared_line | smt_siblings | 9.355 | 9.370 | 11.904 |
| counters | single_writer | one_thread | 4.207 | 4.215 | 4.257 |
| queue | separated_cached | different_cores | 4.025 | 5.111 | 8.116 |
| queue | separated_uncached | different_cores | 16.773 | 17.741 | 18.508 |
| queue | shared_cached | different_cores | 20.217 | 21.244 | 21.835 |
| queue | shared_uncached | different_cores | 13.692 | 18.842 | 20.608 |

The shared-line counters took 8.38 times the separated-line median time per increment on different cores.

The separated, cached queue achieved 3.69 times the sustained throughput of the shared, uncached queue, using the ratio of median ns/item.

The queue includes per-item FIFO validation and full/empty retry counters. It uses uint64 payloads stored in optional slots (see benchmark.log for slot size). These are measurements of this implementation and workload. They are not message latency percentiles, isolated coherence costs, or guarantees for another machine.

Raw data: [samples.csv](samples.csv). Setup and source hashes: [environment.json](environment.json). [Chart](comparison.svg).
