# SPSC queue design

## Contract

One producer constructs values and advances `head`. One consumer moves values
out, destroys the slot contents, and advances `tail`. A thread may change roles
only after synchronization and quiescence; there must never be concurrent calls
on the same side. Queue construction happens before either participant starts.
Destruction and layout inspection require both participants to be stopped.

`Slots` is a power of two. Indices are masked into the array after every update.
`head == tail` is empty and `(head + 1) & mask == tail` is full. One slot remains
unused, so the maximum number of live queued values is `Slots - 1`.

`try_emplace` returns false on an observed full condition without constructing
or consuming its arguments. `try_pop` returns an empty optional on an observed
empty condition. Neither operation waits for the other thread. Concurrent
progress can make a full/empty observation stale immediately; callers choose
whether to retry. There is no stronger promise that a failed try reflects a
simultaneously sampled global state.

## Publication

The producer alone writes `head`, so it can load its own position relaxed. Once
space is established, it constructs the slot's optional value and release-stores
the next head. The consumer's acquire-load of head observes published progress
before it accesses the payload or its engagement flag.

```text
producer: construct payload -> release-store head
                                      |
                                      | synchronizes with an observing acquire
                                      v
consumer:                     acquire-load head -> move payload
```

A later observed head can publish several earlier items. The argument uses
sequenced-before operations and the atomic's coherence, not an assumption that
all hardware behaves like x86.

## Reclamation

The second direction is equally necessary. The consumer first moves the value
into its return object and resets the slot. It then release-stores the next
tail. A producer acquire-load of tail establishes that the consumer has finished
accessing the reclaimed slots before constructing new values there.

```text
consumer: move payload -> destroy slot payload -> release-store tail
                                                       |
                                                       v
producer:                                      acquire-load tail -> reuse slot
```

Removing the release/acquire relation in either direction can allow conflicting
accesses to a non-atomic payload or optional engagement flag. Passing on x86 is
not a language-level correctness argument.

## Cached positions and wraparound

Each side owns a non-atomic cached copy of the remote position. The producer
refreshes tail only when the next head would reach the cached boundary. The
consumer refreshes head only when it reaches the cached end of published data.
The cache can make a thread check earlier than necessary, but cannot authorize
it to cross an unobserved boundary.

Neither side is allowed to lap its cached remote boundary. Before advancing
past it, that side must refresh with acquire ordering. This is why modulo
indices remain usable despite wraparound: a stale value cannot grant a whole
new lap through the ring. Capacity-one, repeated-wraparound, random-reference,
and concurrent-wraparound tests exercise those boundaries.

## Layout experiment

Each cursor contains its atomic position and its owner's cached remote index.
In the shared layout both cursor records fit within one aligned cache line.
In the separated layout each cursor begins at a line boundary. The payload
array is separated from the cursor region in both cases.

The uncached variants retain the unused cache fields. This holds layout fixed
when comparing the index-access strategy. The cached and uncached variants
otherwise share the same queue code through `if constexpr`. Construction,
payload representation, capacity, and ordering semantics are identical.

Separation removes the two writers sharing one index line. It does not remove
the producer reading consumer progress, the consumer reading producer progress,
or the genuine sharing of payload storage. Caching reduces how often remote
positions are read. It does not imply each omitted read would have missed a
cache, nor does padding guarantee a speedup on every workload.

## Ownership and scope

An array of optional values avoids manual raw-storage lifetime management and
supports non-default-constructible, move-only payloads. Full/empty checks occur
before construction or moving. Destruction is nonthrowing. Nothrow payload
operations simplify the API, but those operations are not necessarily bounded,
allocation-free, or fast.

The queue is neither MPSC nor MPMC. It does not include blocking waits,
notifications, cancellation, batching, dynamic capacity, concurrent destruction,
custom allocators, or production telemetry. These features require their own
contracts and evidence. The selected design is sufficient for this experiment.

## References

The [C++ working draft's atomic ordering section](https://eel.is/c++draft/atomics.order)
defines the release/acquire synchronization used in both directions. This is a
living reference; the implementation builds with `-std=c++23` and uses the
release/acquire rules available in that language version. The
[data-race section](https://eel.is/c++draft/intro.races) explains why payload
access requires the resulting happens-before relationships.
