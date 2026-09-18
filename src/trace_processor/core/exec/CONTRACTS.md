# Executor contracts

## Ownership and selection

A RowBatch is a move-only collection of column views and backing owners.
CopyFrom explicitly retains owners and selections without copying owned values.
Published values, validity and selections must remain unchanged for the lifetime
of the view, including across producer advancement, rewind and destruction.
Unowned columns are borrowed until the producer's next call; a retaining
consumer must materialize them. Production sources publish owners.

Finalized dataframe columns retain their existing storage. Sparse expansion,
SQL scans and computed columns publish pooled buffers. A buffer is writable
only when the pool is its sole owner; retained outputs force another allocation.
Value pools cache at most two buffers each. Selection operations use one
reusable slot per produced mapping, with at most two cached buffers per slot.
Slots restart at each operation and retained selections prevent buffer reuse.
String IDs refer to the process-lifetime string pool and do not require copying
string payloads.

Selections map logical rows independently for each column. They may repeat or
reorder rows. Composing selections preserves alignment and owns any index
storage needed after the call. A range can share an existing index owner.

## Retention and compaction

BatchStore retains column views for intrinsic blocking or reordering. Range
views stop at input batch boundaries. Indexed output preserves a run covering
at least half the requested rows as a view; fragmented output keeps columns
sharing backing storage as selections and gathers only columns crossing
buffers. This compaction uses rows already retained by the ordering operator,
so it adds no upstream lookahead and avoids emitting one batch per reordered row.
A dense row-to-batch index costs four bytes per retained row and gives constant-
time lookup even when input batches have different sizes. Retained views are organized by column. Backing and selection properties are
recorded on append, so gathering needs no per-output metadata tables. Columns
in the same row space share physical-row resolution.

BatchBuffer implements optional executor compaction. Columns sharing backing
values and validity concatenate indices. Columns with different backing buffers
are packed independently into pooled storage. Unknown borrowed storage is
materialized before retention. Compaction preserves row order, duplicates,
validity, floating-point bits and per-column alignment.

Stable dataframe storage is not copied merely to reduce retained memory.
Compaction thresholds are explicit policy, not an adaptive cost model.

## Consumer input policy

Each operator declares an InputPolicy; ExecutionOptions supplies the policy for
final output. Layout and batching preferences are independent. The executor
applies them at input boundaries, including after finalization and coalescing.
Any layout preserves producer views. PreferContiguous gathers scattered columns
into execution-local pooled buffers, preserving value bits, nulls and row order.
Range columns keep their backing; indexed contiguous runs become ranges without
copying. Selections whose referenced rows fit within one vector-sized span
remain views, including local filtering and reversal. Wider scatter is packed;
the policy does not use a column-count threshold. Already packed columns are
not copied again. Layout preparation never pulls additional input.

SQLite currently preserves producer views: measurements show that packing helps
wide scattered reads but can regress narrow queries. Consumers must opt in;
there is no universal automatic packing rule.

Final demand is applied before output packing, so a one-row result becomes a
range without materialization. Demand is not pushed as an input cardinality cap
through filtering, expanding or blocking operators. Finite demand still disables
optional batch coalescing at every boundary.

Selection composition shares the result for columns with the same incoming
mapping, even when a computed column separates them. Reusable slot capacity
follows the peak number of mappings, rather than growing with every batch or
allocating all but two mappings repeatedly.

## Execution and demand

Sources distinguish a successful empty batch from exhaustion. The executor
skips empty batches. Operators retain the same input across kHaveMoreOutput
continuations. Finish runs after successful exhaustion, including repeated
Finish calls when it returns kHaveMoreOutput. Errors and cancellation discard
pending compacted output without running successful-exhaustion finalizers.

Latency is the default. A throughput consumer can request small-batch packing;
TreeChildFirst uses this preference because it already blocks intrinsically.
The executor starts packing batches below small_batch_rows and emits at
target_batch_rows, before a larger batch, or on successful exhaustion.

Any finite ExecutionOptions limit disables optional lookahead throughout the
pipeline. Limit zero pulls nothing; limit one returns the first available row
without pulling another batch for packing. This does not remove intrinsic
blocking or prevent evaluation of the current batch. These are executor options,
not additional PipeSQL syntax.

Cancellation is checked between source/operator calls and continuations. It
does not interrupt an individual kernel. Rewind clears pending output, error,
exhaustion and demand counters, and rewinds every node. Retained output remains
valid. Plans and their borrowed dependencies must outlive their executions.

## Validation

Focused executor contract tests cover retained owners and selections, arbitrary
selection composition, pooled reuse, mixed-storage compaction, batch-size
invariance, finite demand, empty batches, failure, cancellation and rewind.
Source/operator tests cover sparse dataframe expansion, direct storage, SQL
scan advancement, conversion and retained computed output. Existing tree tests
continue to validate ordering, malformed trees and accumulation behavior.
