# Experiment results

Machine: Intel(R) Core(TM) i7-10510U CPU @ 1.80GHz. 15 measured rounds per case; 5,000,000 operations per round.

Counters: elapsed wall time until all writers finish / increments per thread. Queue: elapsed wall time until the consumer finishes / delivered items. Thread creation and joins are outside the measured interval. Barrier release and worker scheduling are included.

| Group | Variant | Placement | Min ns/op | Median ns/op | Max ns/op |
| --- | --- | --- | ---: | ---: | ---: |
| counters | same_counter | different_cores | 23.453 | 23.947 | 25.266 |
| counters | separate_lines | different_cores | 4.209 | 4.224 | 4.561 |
| counters | shared_line | different_cores | 23.185 | 24.263 | 28.263 |
| counters | shared_line | smt_siblings | 9.359 | 9.373 | 10.945 |
| counters | single_writer | one_thread | 4.207 | 4.213 | 4.229 |
| queue | separated_cached | different_cores | 5.941 | 7.314 | 11.788 |
| queue | separated_uncached | different_cores | 8.890 | 9.554 | 10.401 |
| queue | shared_cached | different_cores | 16.807 | 19.214 | 21.686 |
| queue | shared_uncached | different_cores | 14.577 | 15.895 | 18.366 |

The shared-line counters took 5.74 times the separated-line median time per increment on different cores.

The separated, cached queue achieved 2.17 times the sustained throughput of the shared, uncached queue, using the ratio of median ns/item.

The queue includes per-item FIFO validation and full/empty retry counters. It uses uint64 payloads stored in optional slots (see benchmark.log for slot size). These are measurements of this implementation and workload. They are not message latency percentiles, isolated coherence costs, or guarantees for another machine.

Raw data: [samples.csv](samples.csv). Setup and source hashes: [environment.json](environment.json). [Chart](comparison.svg).
