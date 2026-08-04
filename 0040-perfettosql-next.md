# PerfettoSQL Next

**Authors:** @LalitMaganti

**Status:** Draft

**PR:** N/A

**Companion RFC:** RFC-0041 (operator reference)

PerfettoSQL Next has two components:

- **Analysis operators**: a small, fixed set of operations for intervals, trees,
  and graphs, which plain SQL does not handle well.
- **Pipe syntax**: a sequence of stages that run in the order they are written.
  This provides a natural place for analysis operators while making queries
  easier to read and audit.

PerfettoSQL Next is fully backwards compatible: all existing PerfettoSQL queries
continue to work unchanged.

This document is the motivation and the design. The grammar, the data model, and
the operator-by-operator detail live in RFC-0041.

## Motivation

### Missing analysis primitives

PerfettoSQL commonly works with slices, heap dumps, stack samples, and
scheduling states. SQL handles relational analysis well, but not many structural
operations required by these datasets.

Consider a basic recursive question. The callstack tree is
`stack_profile_callsite`, one row per callsite with a `parent_id`; the weights
come from `stack_sample`, one row per stack sample, keyed by `callsite_id`. For
a given set of stack samples, compute the flamegraph: the cumulative sum up the
tree, each frame weighted by its own samples plus everything below it. A direct
recursive formulation would sum as it recurses, folding each child's weight into
its parent:

```sql
WITH RECURSIVE
  weights AS (
    SELECT callsite_id, count(*) AS weight
    FROM stack_sample
    GROUP BY callsite_id
  ),
  subtree(id, weight) AS (
    SELECT c.id, coalesce(w.weight, 0)
    FROM stack_profile_callsite c
    LEFT JOIN weights w ON w.callsite_id = c.id
    WHERE c.parent_id IS NULL
    UNION ALL
    SELECT c.id, coalesce(w.weight, 0) + sum(s.weight)
    FROM stack_profile_callsite c
    JOIN subtree s ON s.id = c.parent_id
    LEFT JOIN weights w ON w.callsite_id = c.id
    GROUP BY c.id
  )
SELECT id, weight FROM subtree;
```

This query is invalid because recursive CTEs do not allow aggregate functions or
`GROUP BY` in the recursive term.

SQL instead requires materializing the full descendant closure and then
aggregating:

```sql
WITH RECURSIVE
  weights AS (
    SELECT callsite_id, count(*) AS weight
    FROM stack_sample
    GROUP BY callsite_id
  ),
  descendants(root_id, id) AS (
    SELECT id, id FROM stack_profile_callsite
    UNION ALL
    SELECT d.root_id, c.id
    FROM descendants d JOIN stack_profile_callsite c ON c.parent_id = d.id
  )
SELECT d.root_id AS id, sum(w.weight) AS subtree_weight
FROM descendants d
LEFT JOIN weights w ON w.callsite_id = d.id
GROUP BY d.root_id;
```

This formulation does not scale: the closure contains one row per (node,
descendant) pair, or O(n × depth). A tree of depth 1000 materializes a thousand
rows for every node, and SQLite's recursive CTEs also have a fixed depth limit.
Other subtree aggregations require the same closure.

An interval example is computing the CPU cycles used by `system_server` during a
CUJ. This requires joining scheduling, frequency, and slice data. With the
stdlib's `thread_slice` and `cpu_frequency_counters`:

```sql
INCLUDE PERFETTO MODULE slices.with_context;
INCLUDE PERFETTO MODULE linux.cpu.frequency;

SELECT
  sum(
    (min(s.ts + s.dur, sch.ts + sch.dur, f.ts + f.dur)
     - max(s.ts, sch.ts, f.ts)) * f.freq
  ) AS cpu_cycles
FROM thread_slice s
JOIN sched sch
  ON sch.utid = s.utid
  AND sch.ts < s.ts + s.dur
  AND sch.ts + sch.dur > s.ts
JOIN cpu_frequency_counters f
  ON f.cpu = sch.cpu
  AND f.ts < s.ts + s.dur
  AND f.ts + f.dur > s.ts
  AND f.ts < sch.ts + sch.dur
  AND f.ts + f.dur > sch.ts
WHERE s.process_name = 'system_server'
  AND s.ts >= :cuj_start
  AND s.ts + s.dur <= :cuj_end
```

Even this simple three-way intersection is difficult to read. It requires an
overlap predicate between every pair of tables and a hand-written min/max
expression to calculate the intersection. Adding another interval stream means
adding another join, comparing it with every existing stream, and extending the
min/max expression. This is especially inefficient in SQLite, but remains costly
even in state-of-the-art databases: binary joins do not compose into an
efficient N-way interval intersection.

Trace data commonly has three structural forms:

- **Trees**: slice stacks, callstacks, heap profiles.
- **Graphs**: heap dumps, flow tracking.
- **Intervals**: time ranges, such as slices, scheduling states,
  counters-as-intervals.

These forms are not mutually exclusive. Slices are time intervals, form a tree
when stacked, and form a graph when linked by flows. Analyses need to switch
between these views directly and readably.

### AI-authored queries and human verification

Our audience is systems and application engineers who often use SQL only
occasionally.

AI agents reduce the need for users to write SQL, but their output still needs
human verification. A reviewer must understand both what a query computes and
why. LLM output cannot currently be trusted without review, so generated queries
must remain readable.

SQL's evaluation order does not match its reading order. Code in general is read
top to bottom, but SQL does not work this way:

```sql
SELECT track.name, SUM(slice.dur) AS dur
FROM slice
JOIN track ON slice.track_id = track.id
WHERE ts > 100 AND track.name = 'x'
GROUP BY track.name
HAVING SUM(slice.dur) > 10000
ORDER BY dur DESC
```

is evaluated:

```
FROM/JOIN -> WHERE -> GROUP BY -> HAVING -> SELECT -> ORDER BY
```

### Efficient plans by construction

Agents can produce queries that are logically correct but use a catastrophically
slow formulation. These queries may work on a small trace and fail only when run
on a production-sized trace, and the performance problem is often harder to spot
in review than a semantic error.

High-level analysis operations should exclude known pathological
implementations. Like Rust excludes specific classes of unsafe behaviour from
its safe abstractions, an analysis operation can define semantics without
exposing implementation choices. An agent can request an N-way interval
intersection or a subtree accumulation without choosing a quadratic recursive
closure or a chain of range joins. Existing SQL remains available as an escape
hatch.

### Data Explorer round-tripping

Data Explorer is a graphical interface for building and visualizing trace
queries. A core requirement is bidirectional conversion: users should be able to
construct an analysis graphically, edit the generated query as text, and load it
back into Data Explorer without losing its structure.

Arbitrary SQL cannot support this reliably because the same operation can be
written using different combinations of joins, subqueries, CTEs, and macros.
Recovering the intended operation graph from those forms is ambiguous. Pipe
syntax and a closed operator set give the textual and graphical editors the same
ordered operation model. Except for raw SQL escape hatches, conversion in either
direction is canonical and preserves every operation and parameter.

## Analysis capabilities

The following capabilities cover common operations on intervals, trees, and
graphs. RFC-0041 contains the complete operator reference.

### Intervals

Intervals are central to trace analysis. Three capabilities are required:

- **Construct interval streams.** Combine existing timelines, or turn
  timestamped events into the intervals between them.

  - **Example:** CPU residency is the N-way intersection of frequency and idle
    state intervals. Chained range joins cannot compute this efficiently.
  - **Example:** CPU-frequency samples become state intervals by extending each
    sample until the next sample on the same CPU.

- **Normalize interval streams.** Split intervals at shared boundaries, fill
  gaps, resolve self-overlap, and coalesce adjacent or overlapping spans while
  preserving payload.

  - **Example:** Split scheduling intervals at every frequency change before
    calculating cycles.
  - **Example:** Merge consecutive counter intervals with the same value into a
    single interval.

- **Relate interval streams.** Attach overlapping rows, find the nearest event,
  or keep intervals based on whether they overlap another stream.

  - **Example:** Attach the first presented frame after an input event.
  - **Example:** Keep only slices that overlap a CUJ.

### Trees

Tree operations act on structure rather than independent rows:

- **Filter by structure.** Keep or remove nodes while preserving a valid tree.

  - **Example:** Keep every `doFrame` node and its entire subtree.
  - **Example:** Keep only the paths leading to selected callsites.

- **Merge equivalent nodes.** Unify nodes according to their position and key.

  - **Example:** Merge same-name siblings to turn raw callstacks into a
    flamegraph.
  - **Example:** Merge a `Layout` into a same-name parent to collapse recursion.

- **Accumulate through the tree.** Compute values over each subtree or root
  path.

  - **Example:** Sum samples below every flamegraph frame.
  - **Example:** Propagate a property from each root to its descendants.

- **Reshape the hierarchy.** Fold, prune, regroup, or insert nodes.

  - **Example:** Insert inline frames below their physical callsite.
  - **Example:** Regroup slices around a set of marker nodes.

### Graphs

Graphs occur less often than trees and intervals, but are important for heap
dumps, scheduler dependencies, and flows. The initial graph operator set is
narrow because Perfetto's requirements here are less established.

Most current analyses reduce a graph to a tree and then apply tree operations:

- **Build a dominator tree.** Neither the graph fixpoint nor the subsequent tree
  accumulation is expressible efficiently in plain SQL.

  - **Example:** A dominator tree followed by a subtree sum computes retained
    heap size: which object keeps each allocation alive.

- **Build a traversal tree.** DFS and BFS identify nodes reachable from a seed
  set; BFS also records hop distance.

  - **Example:** A BFS tree shows all work reachable from a scheduling event and
    the number of dependency edges to each node.

Future use cases may justify more graph operations, but we do not yet know which
ones belong in the language.

### Composition

These operations must compose because trace data has several shapes at once. A
slice is both a node in a stack and an interval in time. An analysis might focus
its slice tree on a subtree, accumulate time through that tree, intersect the
remaining slices with scheduling and frequency intervals, and then aggregate the
result with ordinary relational operators.

For example, an analysis can select `doFrame` subtrees, treat the surviving
slices as time intervals, restrict them to periods when their threads were
running, split them at CPU-frequency changes, and calculate CPU cycles. Each
operation should consume the previous result without materializing tables or
converting between separate tree and interval representations.

## Language design

The composition example above requires an explicit operation order. If each
operation were added as a conventional SQL clause, the query might look like
this:

```sql
SELECT s.id, s.name, SUM(dur * freq.freq) AS cpu_cycles
FROM thread_slice AS s
TREE KEEP IF DESCENDANT OF (
  SELECT id FROM thread_slice WHERE name = 'doFrame'
)
INTERVAL SPLIT thread_state AS st PER utid
WHERE st.state = 'Running'
INTERVAL SPLIT cpu_frequency_counters AS freq PER cpu
GROUP BY s.id, s.name;
```

The syntax does not reveal whether `WHERE` runs before or after the tree
operation, whether the two interval splits run in their written order, or where
aggregation occurs relative to either family of analysis operators. Defining a
fixed precedence for every new clause would make these interactions implicit.
Pipe syntax instead places every operation in an explicit sequence.

### Pipe syntax

[GoogleSQL pipe syntax](https://cloud.google.com/bigquery/docs/pipe-syntax)
writes a query as a sequence of steps joined by pipes instead of using the
traditional SQL form.

As in a Unix pipeline, data flows from one stage to the next and each stage
applies an operation.

Example:

```
FROM slice                      -- start: every row of slice
|> WHERE ts > 100               -- keep the rows that match
|> EXTEND dur / 1e6 AS dur_ms   -- add a column
|> SELECT name, dur_ms          -- choose the output columns
```

The composition example now has an unambiguous order:

```sql
FROM thread_slice AS s
|> TREE KEEP IF DESCENDANT OF (
     SELECT id FROM thread_slice WHERE name = 'doFrame'
   )
|> INTERVAL SPLIT thread_state AS st PER utid
|> WHERE st.state = 'Running'
|> INTERVAL SPLIT cpu_frequency_counters AS freq PER cpu
|> AGGREGATE
     SUM(dur * freq.freq) AS cpu_cycles
   GROUP BY s.id, s.name;
```

The tree stage preserves each matching slice and its descendants. The first
interval stage splits those slices at thread-state boundaries, and `WHERE` then
removes time when the thread was not running. Each surviving fragment carries
the CPU from `thread_state`, so the second interval stage can split it at
frequency changes. The final relational stage calculates CPU cycles for each
surviving slice.

Familiar relational operations (`WHERE`, `SELECT`, `EXTEND`,
`AGGREGATE ... GROUP BY`, `JOIN`, `ORDER BY`, `WINDOW`) retain their plain SQL
semantics. The analysis operators use the same pipeline.

### Language integration

#### Canonical StructuredQuery representation

`StructuredQuery`, the existing query representation used by Data Explorer,
becomes the canonical AST for PerfettoSQL Next. The parser converts pipe syntax
to `StructuredQuery`, and serialization produces canonical pipe syntax from the
same representation. Data Explorer reads and edits that AST directly:

```text
Data Explorer
     ↕
StructuredQuery
     ↕
PerfettoSQL Next pipe syntax
```

Every language construct is represented, including relational, interval, tree,
and graph operators; CTEs and forks; pipeline-valued macros; scalar expressions;
table schemas; and declarations. Text and Data Explorer therefore round-trip
through the same operation graph rather than translating between two query
models.

Raw SQL escape hatches are represented as opaque AST nodes. Their text and
position in the pipeline are preserved, but Data Explorer does not interpret or
edit their internals. This is the only part of a query without canonical
structured round-tripping.

#### Embedding pipe queries

Pipe syntax can be used anywhere a SELECT can: in a subquery, in a CTE, as a
table or view body. A pipe query is semantically equivalent to a `SELECT`; there
is no separate pipeline object.

#### GoogleSQL compatibility

Supported relational operators will remain compatible with GoogleSQL pipe
syntax. Unsupported operators will be documented explicitly.

#### Custom operator scope

Only the small, composable core of analysis operators extends the language.

## Execution model

Pipe syntax does not replace SQLite. Relational operations continue to execute
in SQLite.

A transpiler rewrites pipe syntax to SQLite, which executes the relational
operators.

Analysis operators use new C++ implementations exposed to SQLite as intrinsics.
Each operator therefore has a known efficient implementation rather than
expanding into whatever SQL an author happens to write. For example, a dominator
reduction followed by a subtree accumulation invokes those two intrinsics; it
cannot become a quadratic recursive closure. The implementation selects the
execution strategy, so humans and agents do not have to derive a scalable
formulation for every query.

Adjacent tree operators also keep the data in its tree representation. It is
flattened back into rows only when a later operation requires it, avoiding a
conversion before and after every tree stage.

## Evaluation

The evaluation combined authoring studies with a strong LLM, execution against
real traces, cross-engine benchmarks, and an audit of existing stdlib analyses.
It also examined PostgreSQL 18's interval support, DuckDB's interval joins, and
graph databases.

The stdlib audit is necessarily incomplete because analyses that are impractical
in plain SQL may never have been written. The results were:

**Performance is a core justification.**

- Missing-primitive analyses (dominator, reachability, tree fixpoint) are
  intractable in declarative SQL on _any_ engine. DuckDB times out on the same
  recursive-closure formulation a strong LLM naturally writes, because the cost
  is intermediate-result cardinality rather than scan speed.
- N-way interval intersection: the specialised intrinsic is near-linear in the
  number of operands; a general engine's range-join does not compose to N-way
  and degrades by three orders of magnitude on a real 3-way case. Perfetto's
  real interval workloads are N-way.

**The operators compose.** Deep pipe queries are materially shorter and read in
execution order. The flamegraph, which ran to 400 lines as nested calls, is now
a handful of stages.

**Correctness alone does not justify the operators.** A capable author, human or
agent, writes _correct_ plain SQL for individual analyses and even for a deep
hand-composed flamegraph. The operators do not improve correctness in these
cases.

The performance benefit comes from lowering a correct but non-scaling
formulation to an efficient implementation.

**Legibility supports AI authorship and human verification.** Verification
requires reading, so readability directly benefits human reviewers. Efficient
planning by construction remains valuable even when agents generate and iterate
analyses without a human reading every intermediate query.

### Agent learnability

Agent authoring tests produced three results:

- Strong models already write relational pipe syntax without additional
  instruction.
- Given the operator reference, models select and compose stage operators such
  as tree folds and interval reshapes correctly.
- Source operators are harder to learn because they combine several relations
  into a new stream rather than transforming the current stream. Models used
  operations such as N-way interval intersection and event-to-interval
  conversion consistently after receiving two worked examples.

Executable lowering also supports automated evaluation. Each generated pipeline
can be lowered, executed, and compared with stdlib golden results, allowing a
training or evaluation corpus to be labelled without manual review.

## Alternatives considered

Three alternatives were evaluated and rejected.

### Bare SQL functions and macros

RFC-0020 tried exactly this for trees: the tree intrinsics (`tree_filter`,
`tree_propagate_up/down`, `tree_merge_siblings`, `tree_invert`) as plain SQL
functions and macros. Each operation is a function call, so an analysis reads
inside-out and must pass its columns through every nested call;
`viz/flamegraph.sql` requires 400 lines for this composition. Applying the same
API to intervals and graphs would add more relation arguments and nesting. This
RFC retains the intrinsics as lowering targets but replaces the nested
function-call API.

### Typed macros

Typed arguments would make each call less ambiguous, but would not address
composition. Macro arguments become stable API, and nested calls still read
inside-out while passing columns through every stage. Some operations also do
not fit the function-call model: an N-way intersection takes N relations plus a
direction, and there is no natural way to express that as a function of one
stream.

### Standalone analysis language

A standalone language would create two incompatible languages and fragment the
tooling. The UI, the stdlib, the docs, and any agent training would all have to
exist in both worlds. And the analysis operators need the relational core
anyway, so a standalone language would end up rebuilding SQL just to get the
parts it needs. It offers no corresponding benefit because the operators can be
implemented on top of SQL, as described above.

## Out of scope

- Scripting: parameterization, modules versus scripts, and environment. This is
  an orthogonal concern and belongs in its own RFC.

## Open questions

- **Source-operator ergonomics.** Can source operators fit the "transform the
  current stream" model more closely and become easier for agents to learn?
  Some, including N-way interval intersection, inherently create a new stream.
- **Critical / weighted-longest path.** The highest-value scheduling analysis is
  a specialised stdlib primitive today, not an operator. Should it join the
  graph family, or stay a primitive?
- **Keeping the set conservative.** How do we absorb new requests without the
  operator set growing out of control? The likely answer is more intrinsics
  rather than more operators, but it deserves stating.
- **Agent support.** In-context examples and front-end diagnostics are
  sufficient for a strong model. Would cheaper models used for frequent query
  generation benefit from fine-tuning?
