# Experiment protocol

## Hypothesis

Two writers on different cores may pay a substantial cost when their otherwise
independent counters occupy one cache line. In an SPSC queue, separating the
indices may help, but access to the remote index and to payload storage remains.
Compare layout and index caching independently. The winner and effect size are
results to report, not conditions the experiment must satisfy.

## Controls

Counter pairs use relaxed atomic fetch-add. Each requested increment remains
an atomic operation, rather than a non-atomic loop the compiler can collapse
into one final store. Counter values are checked after timing. The true-sharing
control points both threads at the same atomic. The SMT control shares one
physical core, changing more than just cache ownership: execution resources
are shared too, so it is not an isolated measurement of coherence traffic.

The queue comparison is a 2x2: shared/separated indices and uncached/cached
remote positions. All cases use 1024 storage slots, 1023 usable entries, and
uint64 sequence numbers in optional slots. Every delivered value is compared
to its expected sequence number during timing. Full and empty retry counts
are retained. Validation and retries are part of the measured workload.

The benchmark verifies layouts with addresses and fails on a mismatch. These
are explicit runtime checks, not assertions compiled out by `NDEBUG`. The
runner independently verifies actual CPU/core/sibling relationships and the
configured line size through Linux sysfs. Direct binary invocation validates
affinity availability but relies on the caller to establish physical topology;
use the Python runner for recorded experiments.

## Timing

Each run constructs fresh state and starts fresh threads outside the measured
region. Threads pin themselves and meet the coordinator at a barrier. Its
completion function records a `steady_clock` timestamp. Timing therefore
includes release of participants from the barrier and subsequent scheduling.
It excludes allocation, construction, affinity setup, thread creation and joins.

For counters, the end is the latest writer completion timestamp. Divide that
wall time by increments per thread. A two-writer case performs twice that many
increments in total; the CSV rate for counters is a per-writer normalized rate,
not aggregate throughput.

For queues, the end timestamp is recorded immediately after the consumer
validates its last item. Divide wall time by delivered item count. The inverse
is sustained queue throughput. This does not measure individual residence time
or a ping-pong round trip.

The clock is not sampled per item. No cycles are derived from clock-frequency
snapshots. The start barrier is a small fixed overhead whose amortized effect
shrinks as item count rises. Small `--items` values are for smoke testing only.

## Repetitions

Every case receives an untimed 100,000-operation warm-up (or the requested item
count if smaller). Fresh per-round objects mean this is not a guarantee that
every data page starts hot. Warm-up mainly exercises code and brings the
processor under load.

All cases run once in each measured round, in a shuffled order. A recorded seed
makes the ordering reproducible. Raw batch samples are retained before
min/median/max are computed. The seed does not make scheduling or timings
deterministic. Min/max ranges are descriptive spreads, not confidence intervals.

## Limits

This is a laptop experiment. Background work, dynamic frequency, thermal
behavior, scheduler interruptions, and SMT contention can affect results.
Frequency snapshots before and after a run do not establish a steady frequency
inside it. Repeat on another machine before generalizing. Do not silently
discard slow rounds. If a run is interrupted or fails validation, preserve it
as failed and rerun into a new directory.

The runner gives the entire benchmark a 300-second timeout so a regressed
queue cannot leave a run spinning forever. CTest separately bounds tests.
Sanitizers run separately from benchmark measurements. Controlled-schedule
tests establish specific publication/reclamation behavior; their extra
synchronization does not prove the complete memory model correct.

Hardware performance counters are not required and were not assumed available.
Timing changes are consistent with a mechanism; they do not count cache-line
transfers or prove that every remote-index read misses.

## Reproduce a saved run

Read `environment.json` for arguments, compiler flags, selected topology, and
machine state. `source.tar.gz` contains the CMake file, implementation,
tests, and Python scripts. For the bundled September 13 runs, names were
normalized after measurement; `artifact_normalization.original_record_url`
links to the original bytes in Git history. Extract into a new directory, recreate an
appropriate toolchain, and invoke `python3 scripts/run_experiment.py` with the
recorded workload and seed, selecting valid CPUs on your machine. Compare the
shape and spread of the results; identical numbers are not expected.

To regenerate the report without running benchmarks:

```bash
python3 scripts/summarize.py results/RUN_DIRECTORY
```

The summary refuses partial runs or missing/duplicate rounds. The SVG plots
medians and min/max whiskers with separate labeled scales for counters and
queues, since their per-operation definitions differ.
