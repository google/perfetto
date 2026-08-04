# PerfettoSQL Next: Analysis Operators Reference

**Authors:** @LalitMaganti

**Status:** Draft

**PR:** N/A

## Scope

This RFC is the operator reference for PerfettoSQL Next, motivated in RFC-0040.
The **analysis operators**: a closed set of pipe operators for relating data in
**time** (intervals), **structure** (trees), and **dependency** (graphs). They
extend PerfettoSQL's relational pipe operators (§4). The tree operators (§6) are
the RFC-0020 tree functions re-expressed as pipe stages.

## Notation

| notation    | meaning                                              |
| :---------- | :--------------------------------------------------- |
| `UPPERCASE` | a literal keyword                                    |
| _lowercase_ | a user-supplied element (expression, relation, name) |
| `[ ]`       | optional                                             |
| `{ }`       | a required group                                     |
| `\|`        | one of several alternatives                          |
| `, …`       | the preceding element may repeat, comma-separated    |

A **pipe operator** `|> OPERATOR …` transforms the rows from the preceding
stage. Reading order is execution order.

## Contents

1. [Data model](#1-data-model)
2. [Pipeline structure](#2-pipeline-structure)
3. [Shared grammar](#3-shared-grammar)
4. [Relational operators](#4-relational-operators)
5. [Interval operators](#5-interval-operators)
6. [Tree operators](#6-tree-operators)
7. [Graph operators](#7-graph-operators)
8. [Operator summary](#8-operator-summary)
9. [Future operator candidates](#9-future-operator-candidates)
10. [Out of scope](#10-out-of-scope)

---

## 1. Data model

**Interval and event rows.** An **interval row** has two distinguished `INT64`
columns, `ts` and `dur`, denoting `[ts, ts + dur)`; other columns are payload.
An **event row** has `ts` but no `dur`. Any relation exposing `ts`/`dur`
participates in interval operators. (`dur = -1` denotes an interval truncated by
trace end.)

**Trees and graphs: nodes + edges.** A tree and a graph are both a **node**
relation (rows with an `id` and payload) plus an **edge** relation. For a tree
the edges are each node's `parent_id`; for a graph they are `(src, dst)` pairs.
A tree is the acyclic single-parent case. Self-weight lives at _every_ node;
cumulative weight is the subtree sum. Internal ids are dense and never
user-synthesized.

A `slice` row is at once an interval (`ts`/`dur`) and a tree node
(`id`/`parent_id`), so interval and tree operators compose on the same rows.
Most tree operators require the complete tree in the pipe; the filters,
reshapes, and reparent may act on a filtered working set (§6.1).

**Graphs reduce to trees.** A graph's only role is to be reduced to a tree (§7),
carrying node payload onto the result, after which the tree operators take over.

**Payload when a row is split.** A sub-row inherits the original payload. A
**property** (a state) is carried unchanged; a **measure** (bytes, energy, a
count) must be rescaled by an expression in a following `EXTEND`. The language
never infers which.

---

## 2. Pipeline structure

```
create_table :=
    "CREATE PERFETTO TABLE" name [ "(" schema ")" ] "AS" pipeline_expr ;

pipeline_expr := source { "|>" stage } ;
fork          := "FORK AS" name ;

create_macro :=
    "CREATE PERFETTO MACRO" name "(" [ macro_param { "," macro_param } ] ")"
    "RETURNS" "Pipeline" "AS" "(" pipeline_body ")" ;
macro_param  := name ( "TableOrSubQuery" | "ColumnName" | "Expr" ) ;
pipeline_body := pipeline_expr | { "|>" stage } ;
```

A named, materialized result uses the existing `CREATE PERFETTO TABLE ... AS`
statement with a pipeline expression as its body. Its optional schema validates
the output columns and types; it does not project or reorder them. A final stage
must therefore produce the declared columns, although their order is not
significant.

Every input carries an alias; columns are `alias.col`.

**Kinds and consumption.** Every pipeline expression has a kind, one of
`Relation`, `Tree`, `Graph`, or `IntervalSet`, determined by its source and
stages. Typing is strict inside an expression: passing a `Graph` where an
interval operator expects an `IntervalSet` is an error. A pipeline expression
can appear anywhere a `SELECT` can. At that boundary, a non-`Relation` result
flattens to its underlying rows. There is no named pipeline object or standalone
pipe statement.

A reusable pipeline with its own source is an ordinary CTE:
`WITH name AS (pipeline_expr)`. It follows normal CTE scope and naming rules and
may reference earlier CTEs and base tables. `FORK AS name` instead names the
_current_ stream at that point so later stages can reference both that snapshot
and its subsequent transformations; the stream passes through unchanged.

For example, this preserves the unmerged intervals as `raw_states`, merges the
current stream, and then attaches every original interval contained by each
merged interval:

```sql
FROM thread_state
|> FORK AS raw_states
|> INTERVAL MERGE CONSECUTIVE BY state PER utid
   AGGREGATE COUNT(*) AS fragment_count
|> INTERVAL JOIN raw_states AS raw WITHIN BOUNDS PER utid
```

A CTE cannot replace this inline snapshot without repeating the pipeline prefix.

A **pipeline-valued macro** (`RETURNS Pipeline`) is a parameterized pipeline
fragment, the one way to parameterize a pipeline by an input _relation_. Its
parameters may be relations (`TableOrSubQuery`), columns (`ColumnName`) or
scalar expressions (`Expr`), referenced in the body as `$name`. Its body is
either a full `pipeline_expr` (with a source), invoked as a source,
`name!(args)`, or a tail of `|>` stages, invoked mid-pipe, `… |> name!(args)`.
It expands in place. This is how operations that take a relation argument (clip
to a caller-supplied window, an N-ary aggregation over a passed-in window) are
written without leaving the pipe surface.

---

## 3. Shared grammar

**`PER cols`** confines an operator to each key group independently.

**Anchor** (closed), an endpoint or extent of the current row or a named
relation:

```
anchor := ( "BEGIN" | "END" | "BOUNDS" ) [ "OF" rel ] ;
```

**Matching key** (closed), shared key columns, assumed equal on both sides,
never a predicate:

```
by_clause := "BY" cols ;
```

**Aggregate vocabulary** (closed), decomposable-only; carried by every operator
that collapses rows (`FLATTEN`, `MERGE`, `MERGE SIBLINGS`, `MERGE INTO PARENT`)
and by `ACCUMULATE`:

```
agg := "COUNT" "(" "*" ")"
     | ( "SUM" | "MIN" | "MAX" | "AVG" ) "(" expr ")"
     | ( "ARG_MAX" | "ARG_MIN" ) "(" expr "," expr ")"
     | "GROUP_CONCAT" "(" expr ")" ;

agg_clause := "AGGREGATE" agg "AS" name { "," agg "AS" name } ;
```

---

## 4. Relational operators

PerfettoSQL's relational pipe operators; syntax inspired by the GoogleSQL pipe
and query syntax, engine is SQLite. Each is a stage `|> OPERATOR …`, except
`FROM`.

| operator                                       | effect                                          |
| :--------------------------------------------- | :---------------------------------------------- |
| `FROM rel`                                     | start a pipeline from a table or subquery       |
| `WHERE expr`                                   | keep rows where `expr` is true                  |
| `SELECT cols`                                  | choose / compute the output columns             |
| `EXTEND expr AS name`                          | add computed columns, keeping the existing ones |
| `SET col = expr`                               | replace a column's value                        |
| `DROP col, …`                                  | remove columns                                  |
| `RENAME col AS name`                           | rename columns                                  |
| `DISTINCT`                                     | deduplicate rows                                |
| `AGGREGATE agg AS name [ GROUP BY cols ]`      | group and aggregate                             |
| `ORDER BY cols [ ASC \| DESC ]`                | sort                                            |
| `LIMIT n [ OFFSET m ]`                         | take a prefix of rows                           |
| `JOIN rel ON expr` / `USING (cols)`            | value join                                      |
| `UNION` / `INTERSECT` / `EXCEPT` `[ ALL ] (…)` | set operations                                  |
| `WINDOW expr AS name`                          | window functions (also inline via `OVER`)       |

`AGGREGATE` and `OVER` are SQLite aggregation, distinct from the closed
`agg_clause` (§3) of the collapsing analysis operators.

`AGGREGATE … GROUP BY cols` outputs the `GROUP BY` columns first (in order),
then the named aggregates (in order), the GoogleSQL-pipe contract. So a
following `|> SELECT <those same columns, same order>` is a no-op; a `SELECT` is
only needed to rename, reorder, compute, or drop columns.

A `GROUP BY` entry is `expr [ AS name ]`. A key appears only in `GROUP BY`,
never restated in the body; anything functionally determined by a key is itself
a key (`GROUP BY c.cpu, c.idle + 1 AS state`).

Every body expression must be an aggregate, no bare columns. For a value
constant within the group use `ANY_VALUE(col)`; for a value from the row at an
extremum use `ARG_MAX(ordering, value)` / `ARG_MIN(ordering, value)` (not
`ANY_VALUE`, which picks an arbitrary row). These are available wherever SQLite
aggregation is.

### Raw SQL stage

When no operator expresses what is needed, a raw-SQL stage transforms the
current stream like any other stage, reading its input through the `@input`
handle. This is the boundary with no cliff edge: dropping to SQL is an ordinary
stage, not a mode switch.

```
FROM slice
|> WHERE name GLOB 'm*'
|> SQL (
     SELECT *, ROW_NUMBER() OVER (PARTITION BY name ORDER BY ts) AS rn
     FROM @input
   )
|> WHERE rn = 1
```

`@input` is the only sigil in the language and is available only inside a raw
SQL stage. The exact keyword for the stage is not yet fixed.

---

## 5. Interval operators

### 5.1 Sources

#### `INTERVAL INTERSECTION OF` / `UNION OF`

```
source := "INTERVAL" ( "INTERSECTION" | "UNION" ) "OF" "(" rel { "," rel } ")" [ "PER" cols ] ;
```

`INTERSECTION OF` emits a fragment over each region where **all** inputs
overlap; `UNION OF` over each region where **any** input covers. Both
co-fragment the operands side by side: each output fragment carries every
operand's columns, null where that operand is absent.

```
WITH cpu_residency AS (
  INTERVAL INTERSECTION OF (cpu_freq AS f, cpu_idle AS i) PER cpu
  |> AGGREGATE SUM(dur) AS ns GROUP BY cpu, f.freq, i.state
)
SELECT * FROM cpu_residency;
```

#### `INTERVALS FROM EVENTS`

```
source := "INTERVALS FROM EVENTS" rel [ "PER" cols ] [ "STOP" rel ]
          [ "KEEPING LAST OPEN" | "CLOSING LAST AT" "(" expr ")" ] ;
```

Each event opens an interval closed by the next event in its lane (or a `STOP`
event). The final open interval is retained with `dur = -1`
(`KEEPING LAST OPEN`) or closed at `CLOSING LAST AT (expr)`; the `expr` may
reference the lane's columns (e.g. a per-process end).

Events are **not** coalesced, a run of equal-valued samples yields one interval
each; collapse them with a following `MERGE CONSECUTIVE BY value` (§5.2), which
carries the keyed columns through. Each interval carries its _opening_ event's
payload; the closing event's columns (a transition reason, the next value, a
delta) are the next event's, reached with §4 windowing over lane order.

```
INTERVALS FROM EVENTS cpu_freq_events PER cpu CLOSING LAST AT (trace_end())
|> EXTEND value - LAG(value) OVER (PARTITION BY cpu ORDER BY ts) AS delta
```

### 5.2 Reshaping

#### `INTERVAL SPLIT`

```
stage := "|>" "INTERVAL SPLIT" rel "AS" alias [ "PER" cols ] ;
```

Cuts each current row at the operand intervals' boundaries, one sub-interval per
piece, attaching the operand's columns (null where no operand is present).
Lossless, the sub-intervals tile the original.

```
FROM slice AS s
|> INTERVAL SPLIT thread_state AS st PER utid
|> AGGREGATE SUM(dur) AS dur GROUP BY s.id, st.state;
```

#### `INTERVAL SUBTRACT`

```
stage := "|>" "INTERVAL SUBTRACT" rel [ "PER" cols ] ;
```

Removes the operand's coverage; the pieces _outside_ the operand survive.

#### `INTERVAL FILL`

```
stage := "|>" "INTERVAL FILL" "WITHIN" rel [ "PER" cols ] ;
```

Fills the gaps in the current stream: every input interval passes through, and a
null-payload filler is added over each span of `WITHIN` the stream does not
cover, per lane, so the output tiles `WITHIN` completely. (For the gaps _alone_,
subtract the stream from `WITHIN` with `INTERVAL SUBTRACT`.)

```
FROM thread_state AS s
|> INTERVAL FILL WITHIN trace_bounds PER utid
```

#### `INTERVAL FLATTEN`

```
stage := "|>" "INTERVAL FLATTEN" [ "PER" cols ] agg_clause ;
```

Cuts a self-overlapping set at every internal boundary into disjoint segments
and collapses the rows live in each segment into one row via `agg_clause`. Emits
nothing over uncovered time.

```
FROM slices
|> INTERVAL FLATTEN PER cpu
   AGGREGATE COUNT(*) AS concurrency, ARG_MAX(depth, name) AS deepest;
```

#### `INTERVAL MERGE`

```
stage := "|>" "INTERVAL MERGE OVERLAPPING" [ "PER" cols ] agg_clause
       | "|>" "INTERVAL MERGE CONSECUTIVE BY" cols [ "PER" cols ] agg_clause ;
```

`OVERLAPPING` coalesces overlapping and abutting intervals into coverage;
`CONSECUTIVE BY cols` merges only adjacent intervals agreeing on `cols`. The
merged payload is combined by `agg_clause`.

```
FROM thread_state
|> INTERVAL MERGE CONSECUTIVE BY state AGGREGATE SUM(bytes) AS bytes;
```

#### `INTERVAL SET BOUNDS`

```
stage := "|>" "INTERVAL SET BOUNDS" "BEGIN AT" expr ( "END AT" expr | "FOR" expr ) [ "PER" cols ] ;
```

Replaces `ts`/`dur` with new bounds from payload expressions.

```
FROM binder AS b
|> INTERVAL SET BOUNDS BEGIN AT b.client_ts END AT b.server_ts
```

#### `INTERVAL QUANTIZE`

```
stage := "|>" "INTERVAL QUANTIZE" width [ "ALIGNED TO" ( "TRACE START" | "ZERO" | expr ) ] [ "PER" cols ] ;
```

Cuts rows into fixed-width buckets (splitting at boundaries) and tags each piece
with a `bucket` column. `ALIGNED TO` sets the origin (default `TRACE START`).

```
FROM sched
|> INTERVAL QUANTIZE 100ms PER cpu
|> AGGREGATE SUM(dur) AS cpu_ns GROUP BY cpu, bucket;
```

### 5.3 Matching

The relationship phrase:

```
relationship := "OVERLAPPING" "BOUNDS"
              | "COVERING" ( "BEGIN" | "END" | "BOUNDS" )
              | "WITHIN"   "BOUNDS" ;
```

`COVERING`, the operand covers my point/extent; `WITHIN`, the operand lies
inside my extent; `OVERLAPPING`, the operand overlaps my extent. A dur-less
event row is a zero-width point, so `COVERING BEGIN` matches the operand
containing it.

#### `INTERVAL JOIN`

```
stage := "|>" [ "LEFT" ] "INTERVAL JOIN" rel "AS" alias relationship [ "PER" cols ] ;
```

Attaches matched operand columns without changing the current rows' bounds;
multiplies per match. `LEFT` keeps unmatched rows.

```
FROM frames AS f
|> INTERVAL JOIN cujs AS c COVERING BOUNDS
```

#### `INTERVAL FIND`

```
stage := "|>" [ "LEFT" ] "INTERVAL FIND" rel "AS" alias
         ( "STARTING" | "ENDING" ) ( "AFTER" | "BEFORE" ) anchor
         [ "WITHIN" dur ] [ "PER" cols ] ;
```

Attaches the single operand row nearest in the given direction. At most one
matches; `LEFT` yields nulls when none does. `WITHIN dur` bounds the search, the
nearest within the tolerance, otherwise no match.

```
FROM input_events AS i
|> LEFT INTERVAL FIND frames   AS f STARTING AFTER END
|> LEFT INTERVAL FIND presents AS p STARTING AFTER END OF f
|> EXTEND p.ts - i.ts AS latency;
```

#### `INTERVAL KEEP IF` / `DROP IF`

```
stage := "|>" "INTERVAL" ( "KEEP" | "DROP" ) "IF" relationship rel ;
```

The temporal semijoin / antijoin. Matching is against the operand's merged
coverage, so a row related to several operand intervals survives once.

```
FROM slice
|> INTERVAL KEEP IF OVERLAPPING BOUNDS janks
```

---

## 6. Tree operators

The tree operators are RFC-0020's tree functions re-expressed as pipe stages.
`TREE KEEP IF` is `tree_filter`; `TREE ACCUMULATE UP/DOWN` is
`tree_propagate_up/down`; `TREE MERGE SIBLINGS` is
`tree_merge_siblings[_ordered]`; `TREE MERGE INTO PARENT` is
`tree_merge_into_parent`; `TREE INVERT` is `tree_invert`. `CONTRACT`, `ABSORB`,
`PRUNE`, `REPARENT`, and `EXPAND` are new.

### 6.1 Structure and weight

The **filters** (§6.2), **reshapes** (§6.3), and **reparent** (§6.4) may act on
a filtered working set, naming the full structure with `OVER rel`. The **folds**
(§6.5-6.6), **`ACCUMULATE`** (§6.8), and **`EXPAND`** (§6.9) require the
complete tree in the pipe.

```
over_clause := "OVER" rel ;
```

**Measures and properties.** Columns named in a fold's `agg_clause` are
**measures**: they relocate to the operator's destination (parent, self, or
merge target) and combine by the stated agg. Every unnamed column is a
**property**: it stays unchanged with its node. Inserts (`EXPAND`) carry no
`agg_clause`, new nodes are zero unless valued by their source relation, and
`CHARGE` moves one measure to the far end. A scalar can be folded together but
never split apart, so any operation that fans a measure out happens at
construction, before aggregation.

### 6.2 Filters

```
structural_rel := ( "ANCESTOR" | "DESCENDANT" ) "OF" rel ;
stage := "|>" "TREE" ( "KEEP" | "DROP" ) "IF" structural_rel [ over_clause ] ;
```

The structural semijoin / antijoin. `rel` is a **set of rows** (often a filtered
set), not a predicate. `KEEP IF DESCENDANT OF sel` keeps subtrees under `sel`
(focus); `KEEP IF ANCESTOR OF sel` keeps paths to `sel`;
`DROP IF DESCENDANT OF sel` prunes them.

```
FROM slice
|> TREE KEEP IF DESCENDANT OF (SELECT id FROM slice WHERE name = 'doFrame')
```

### 6.3 Reshapes

```
stage := "|>" "TREE" ( "CONTRACT" | "ABSORB" ) "AT" rel [ over_clause ] agg_clause
       | "|>" "TREE PRUNE AT" rel [ over_clause ] ;
```

`AT rel` selects the nodes (by `id`). `CONTRACT` removes each, reparents its
children up, and charges its measures to the parent. `ABSORB` keeps each, drops
its subtree, and folds the descendant measures into self. `PRUNE` removes each
node and its whole subtree; its measures are discarded, so it takes no
`agg_clause`.

```
FROM slice
|> TREE CONTRACT AT (SELECT id FROM slice WHERE name GLOB '*__internal*')
   AGGREGATE SUM(self_dur) AS self_dur
```

### 6.4 Reparent

```
stage := "|>" "TREE REPARENT TO NEAREST ANCESTOR IN" rel [ over_clause ] ;
```

Row-preserving: each node's parent becomes its nearest strict ancestor that is
in `rel`, the marker set, yielding a forest with one subtree per marker. Passing
the boundary markers of a key column recovers a partition-by-group.

### 6.5 Merge siblings

```
stage := "|>" "TREE MERGE SIBLINGS BY" cols [ "ORDERED" ] agg_clause ;
```

`MERGE SIBLINGS BY cols` re-keys every node by its root-to-node path of `cols`
and unifies nodes with the same path, combining payloads via `agg_clause`,
taking a tree of callstacks to a tree keyed by name path (a flamegraph).
`ORDERED` unifies only _consecutive_ same-key runs.

```
FROM callstacks
|> TREE MERGE SIBLINGS BY name AGGREGATE SUM(self_weight) AS self_weight
|> TREE ACCUMULATE UP SUM(self_weight) AS total;
```

### 6.6 Merge into parent

```
stage := "|>" "TREE MERGE INTO PARENT BY" cols [ "INDIRECT" ] agg_clause ;
```

Collapses a node into a same-key parent, combining payloads via `agg_clause`.
`INDIRECT` also folds intermediary frames between two occurrences.

### 6.7 Invert

```
stage := "|>" "TREE INVERT" ;
```

Flips leaves and roots.

### 6.8 Accumulate

```
stage := "|>" "TREE ACCUMULATE" ( "UP" | "DOWN" ) agg "AS" name { "," agg "AS" name } ;
```

Row-preserving; one value per node. `UP` aggregates each node's **subtree**
(cumulative) using the decomposable aggregates of §3. `DOWN` aggregates each
node's **root-path**, the ordered chain root→node, and so additionally admits
the ordered-path reductions `FIRST` / `LAST` (e.g. inherit the nearest non-null
ancestor's value); `UP` cannot, because a subtree has no order.

```
FROM slice
|> TREE ACCUMULATE UP SUM(self_dur) AS total;
```

### 6.9 Tree expansion

```
stage := "|>" "TREE EXPAND" ( "UP" | "DOWN" ) rel "BY" cols [ "ORDERED BY" cols ]
         [ "CHARGE" cols "TO" ( "LEAF" | "ROOT" ) ] ;
```

Inserts a keyed sub-structure `rel` above (`UP`) or below (`DOWN`) each matched
node; `rel` carries its own internal `parent_id`, so its shape is free, a chain,
a flat layer (the former `ATTACH`), or any forest. The matched node survives at
its end (leaf-ward for `UP`, root-ward for `DOWN`) and its existing subtree
reconnects at the far end. Inserted nodes are zero unless `rel` values them.

`CHARGE cols TO ( LEAF | ROOT )` migrates the named measures from the matched
node to the far end the insertion created: `DOWN` admits only `TO LEAF`, `UP`
only `TO ROOT`, the other pairing is an error. It is well-defined only when the
far end is a single node (a chain); a fan-out has no single target, so those
measures must be valued in `rel` instead.

```
-- Inline expansion: each callsite becomes a chain of inline frames,
-- its self charged to the deepest frame.
FROM stack_profile_callsite
|> FORK AS callsites
|> … build the callstack tree …
|> TREE EXPAND DOWN (FROM callsites |> … derive inline frames …)
   BY callsite_id ORDERED BY inline_depth CHARGE self TO LEAF
```

---

## 7. Graph operators

A graph is reduced to a tree, which the tree operators then analyze.

```
source := "GRAPH" ( "DFS" | "BFS" | "DOMINATOR" ) "TREE"
          "NODES" rel "EDGES" rel "FROM" rel ;
```

| operator               | tree built                                       |
| :--------------------- | :----------------------------------------------- |
| `GRAPH DFS TREE`       | depth-first spanning tree from the seeds         |
| `GRAPH BFS TREE`       | breadth-first / shortest-hop tree from the seeds |
| `GRAPH DOMINATOR TREE` | dominator tree from the roots                    |

`NODES rel` carries node payload onto the tree; `EDGES rel` is the `(src, dst)`
relation; `FROM rel` is the seeds. `DOMINATOR` accepts a multi-row `FROM` (a
forest of roots, e.g. GC roots): a virtual super-root is inserted over them and
stripped from the result, so no id is user-synthesized. `DFS` and `BFS` give the
same node-set, different trees; that shared node-set **is** the set reachable
from the seeds. All three dedup; ties are broken by node `id` so the tree is
deterministic.

A _single-hop_ neighbour lookup over an edge relation is a plain §4 `JOIN` on
`EDGES`, not a graph operator; only transitive reduction lives here.

```
-- Heap retained size
GRAPH DOMINATOR TREE NODES heap_objects EDGES heap_refs FROM gc_roots
|> TREE ACCUMULATE UP SUM(self_size) AS retained;
```

---

## 8. Operator summary

| operator                                | family           | effect                                            |
| :-------------------------------------- | :--------------- | :------------------------------------------------ |
| `INTERVAL INTERSECTION OF` / `UNION OF` | interval source  | combine N coverages (all / any)                   |
| `INTERVALS FROM EVENTS`                 | interval source  | events → intervals                                |
| `INTERVAL FILL`                         | interval reshape | pass input through + fill gaps to cover an extent |
| `INTERVAL SPLIT`                        | interval reshape | fragment at operand boundaries; lossless          |
| `INTERVAL SUBTRACT`                     | interval reshape | keep the non-operand pieces                       |
| `INTERVAL FLATTEN`                      | interval reshape | resolve self-overlap; collapse each segment       |
| `INTERVAL MERGE`                        | interval reshape | coalesce to coverage / runs                       |
| `INTERVAL SET BOUNDS`                   | interval reshape | re-anchor `ts`/`dur` from payload                 |
| `INTERVAL QUANTIZE`                     | interval reshape | fixed-width buckets                               |
| `INTERVAL JOIN`                         | interval match   | attach matched; multiplies                        |
| `INTERVAL FIND`                         | interval match   | attach the one nearest                            |
| `INTERVAL KEEP IF` / `DROP IF`          | interval match   | semijoin / antijoin                               |
| `TREE KEEP IF` / `DROP IF`              | tree filter      | focus / prune                                     |
| `TREE CONTRACT` / `ABSORB`              | tree reshape     | fold a node up / fold its subtree in              |
| `TREE PRUNE`                            | tree reshape     | drop node and its subtree                         |
| `TREE REPARENT`                         | tree reshape     | reparent to nearest marker ancestor               |
| `TREE MERGE SIBLINGS`                   | tree fold        | unify by path key (build a flamegraph)            |
| `TREE MERGE INTO PARENT`                | tree merge       | collapse recursion                                |
| `TREE INVERT`                           | tree reorient    | flip leaves ↔ roots                               |
| `TREE ACCUMULATE UP` / `DOWN`           | tree aggregate   | subtree / root-path fold                          |
| `TREE EXPAND UP` / `DOWN`               | tree construct   | insert a keyed sub-structure; charge to far end   |
| `GRAPH DFS / BFS / DOMINATOR TREE`      | graph source     | reduce a graph to a tree                          |

---

## 9. Future operator candidates

- **Weighted / longest path** (e.g. scheduling critical path), currently a
  specialized stdlib primitive, not a `GRAPH … TREE` operator.
- **Recovering the full structure** from a filtered set of nodes (§6.1).
- **Structural `FIND`**, attach the nearest descendant/ancestor matching a set
  onto a node (the tree twin of `INTERVAL FIND`).
- **Property propagation** across grafted or joined trees.
- **Interval `OVERLAY`**, priority/z-order compositing (a higher lane replaces a
  lower one on overlap, both survive outside); `MERGE` cannot hole-punch.
- **Mid-pipe clip** to a window or operand extent (`INTERSECTION OF` is a source
  only); cross-lane bounds and greedy start↔end stream pairing.

---

## 10. Out of scope

These belong to §4 relational operators or the host language, not the analysis
families:

- **Wide pivot**, long-form series → one column per value (`battery`,
  `runnable`): §4 conditional aggregation; SQLite has no `PIVOT`.
- **Neighbour / delta / rate** over a lane: §4
  `LAG`/`LEAD … OVER (PARTITION BY lane ORDER BY ts)`.
- **Single-hop graph neighbour**: a §4 `JOIN` on the edge relation (§7).
- **Non-decomposable window reductions**, runtime table-name dispatch, scalar
  pipelines, and native `__intrinsic_*` escape hatches.
