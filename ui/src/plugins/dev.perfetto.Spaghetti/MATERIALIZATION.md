# How Spaghetti materialization works

This doc follows one graph through every stage of the pipeline. All plans,
hashes and SQL below are **real output from the current code**, not
hand-written examples.

```mermaid
flowchart LR
  G["Graph (store)"] --> P["1. buildPlan<br/>plan.ts"]
  P --> M["2. planMaterialization<br/>materialization.ts"]
  M --> C["3. compose<br/>compose.ts"]
  C --> X["4. Executor + cache<br/>MaterializationService"]
  P --> D["buildDisplayQuery<br/>(SQL tab)"]
```

| Stage | Pure? | Input | Output |
|---|---|---|---|
| Node generators | yes | config + input references | SQL / PSN fragment |
| 1. `buildPlan` | yes | graph + target node | `Plan`: nodes in topological order (no hashes) |
| 2. `planMaterialization` | yes | `Plan` + dialect | steps (one cache table each, hashed from their SQL) + output query |
| 3. `compose` (`formatStep`, `formatOutput`) | yes | a step or the output | one SQL string |
| 4. Executor | **no** | materialization plan + cache | tables in TraceProcessor, DataGrid source |

Everything before the executor is a pure function of the graph, the target and
the dialect. The cache only affects **which steps run**, never the plan.

---

## The example graph

A single docked chain, with the **Limit** node selected:

```
From(slice)
  → Filter(dur > 1000)
  → Extend(dur / 1e6 AS dur_ms)
  → GroupBy(name; COUNT(*) AS cnt, SUM(dur_ms) AS total_ms)   ← expensive
  → Filter(cnt > 5)
  → Sort(total_ms DESC)                                        ← expensive
  → Limit(10)                                                  ← selected
```

Node ids: `src`, `flt1`, `ext`, `gb`, `flt2`, `srt`, `lim`.

---

## 0. Node generators

Each node manifest has `generators: {sql, psn?}`, plus an optional
`expensive: true`. Generators are pure. They receive the node's config and a
context giving access to the inputs (`ctx.input(port)`, `ctx.inputs()`) and
their columns.

**SQL fragments are structured clauses**, never full SQL strings to be parsed:

```ts
filter:  {where: 'dur > 1000'}
extend:  {select: ['*', 'dur / 1e6 AS dur_ms']}
groupby: {groupBy: ['name'], select: ['name', 'COUNT(*) AS cnt', 'SUM(dur_ms) AS total_ms']}
sort:    {orderBy: ['total_ms DESC']}
limit:   {limit: 10}
from:    {from: 'slice'}                       // a bare name: a plain reference
ii:      {from: '_interval_intersect!((a, b), (utid))', includes: [...]}
union:   {from: '(SELECT * FROM a UNION ALL SELECT * FROM b)'}
```

**PSN fragments are a source and/or stages:**

```ts
filter:  {stages: ['WHERE dur > 1000']}
from:    {source: 'FROM slice'}
ii:      {source: 'INTERVAL INTERSECTION OF (...)', stages: ['SELECT ...']}
union:   {stages: ['UNION ALL (...)']}  // continues the first input's pipeline
```

A generator may also return:
- **`undefined`**: the node is incomplete, e.g. a required input isn't
  connected.
- **`{error}`**: the node can't be expressed in this dialect.
- **`{}`**: an empty fragment, meaning a passthrough.

The SQL generator is called **twice** per node:
- **During planning, as a probe**, with placeholder references, only to find
  incomplete nodes and passthroughs.
- **During composition**, with real references: CTE names, `_qb_*` tables, or
  inline pipelines.

---

## 1. Graph → Plan (`plan.ts`)

`buildPlan(index, targetId)` walks **upstream** from the target depth-first,
memoized per node, with cycle detection. For each node it:

1. Evaluates the node's inputs first, recursively.
2. Probes `generators.sql(config, ctx)` and handles the result:
   - `undefined`: incomplete, so the target gets no plan, which the UI shows as
     "Incomplete query".
   - `{}`: a passthrough. The node forwards its primary input and drops out of
     the plan.
   - anything else: the node is kept.
3. Records its inputs, its input columns, its `expensive` flag and its includes.

**The plan contains no hashes.** It's pure structure:

```
  src   from     expensive=false refCount=1 inputs=[]
  flt1  filter   expensive=false refCount=1 inputs=[src]
  ext   extend   expensive=false refCount=1 inputs=[flt1]
  gb    groupby  expensive=true  refCount=1 inputs=[ext]
  flt2  filter   expensive=false refCount=1 inputs=[gb]
  srt   sort     expensive=true  refCount=1 inputs=[flt2]
  lim   limit    expensive=false refCount=0 inputs=[srt]
```

---

## 2. Plan → MaterializationPlan (`materialization.ts`)

### Steps

**There is one rule:** the output of every `expensive` node becomes a cache
table. A **step** is an expensive node plus the cheap nodes behind it, back to
the previous expensive nodes, which become its `deps`. Whatever is left in front
of the target is the **output**.

### Hashes: the cache key is the query that builds the table

`computeStepHashes(plan, dialect)` visits expensive nodes in topological order.
For each one it composes the step's query **in the active dialect**, with every
dependency table named by that dependency's own hash, and hashes the result:

```ts
hash = `_qb_${fnv(dialect + '\n' + stepSql)}`
```

This is a Merkle hash without any extra machinery:
- **Upstream changes propagate.** A dependency's name *is* its hash, so any
  upstream change renames it, which changes this step's text and so its hash.
- **The key is exactly what runs.** "Same hash" means "same query", and the
  Cache tab's stored SQL is the text the hash was computed from.
- **Node ids, layout and UI-only config don't matter.** Only the generated SQL
  is hashed.
- **Each dialect has its own tables.** The dialect is part of the hashed text,
  so Standard SQL and PSN never share a cache table.

For the example in Standard SQL:

```
  step _qb_324566e1 root=gb  nodes=[src,flt1,ext,gb] deps=[]
  step _qb_171cdcd6 root=srt nodes=[flt2,srt]        deps=[_qb_324566e1]
  output            root=lim nodes=[lim]             deps=[_qb_171cdcd6]
```

and in PSN (same steps, different hashes):

```
  step _qb_ba3e9372 root=gb  nodes=[src,flt1,ext,gb] deps=[]
  step _qb_f5c00c00 root=srt nodes=[flt2,srt]        deps=[_qb_ba3e9372]
  output            root=lim nodes=[lim]             deps=[_qb_f5c00c00]
```

```mermaid
flowchart LR
  subgraph S1["step _qb_324566e1"]
    src --> flt1 --> ext --> gb
  end
  subgraph S2["step _qb_171cdcd6"]
    flt2 --> srt
  end
  subgraph O["output"]
    lim
  end
  gb --> flt2
  srt --> lim
```

Steps are listed in dependency order, so each step comes after the steps it
reads. Two identical subgraphs produce the same SQL, get the same hash and
share one step, whose `nodeIds` lists both sets of nodes.

The plan never looks at the cache: the same graph and dialect always give the
same steps and hashes.

---

## 3. Compose (`compose.ts`)

`formatStep(matPlan, step)` renders one step as a query body in the plan's
dialect; the executor adds the `CREATE PERFETTO TABLE` itself.
`formatOutput(matPlan)` renders the output, **always in Standard SQL**, because
the DataGrid wraps it in plain SQL, which can't contain a pipe query. Nodes
outside the step being rendered are referred to by table name: `_qb_<hash>`, or
a temp name supplied by `graph_check`/`node_eval`.

How a query is rendered depends **only on the nodes inside it**. Consumer counts
are taken within the query being built, so a step's SQL, and therefore its hash,
doesn't change when unrelated nodes are added elsewhere on the canvas.

### Standard SQL: clause-order folding

Each clause has a fixed position in SQL's evaluation order:

```
FROM(0) → WHERE(1) → GROUP BY(2) → SELECT(3) → DISTINCT(4) → ORDER BY(5) → LIMIT/OFFSET(6)
```

A node's fragment **folds** into its primary input's open block only if all of
these hold:
- the input is used once **within this query**;
- the fragment has no `from`;
- every clause it adds comes **strictly later** than the block's latest clause.
  The one exception is `WHERE` + `WHERE`, which is ANDed while the block hasn't
  got past `WHERE`.

Otherwise the input block is written out as a **CTE** named after its node type
(`filter_1`, `extend_1`…), and a new block reads from it.

Step `_qb_324566e1`:

| Node | Fragment adds | Block's latest clause | Result |
|---|---|---|---|
| src | `from: slice` | n/a | block `{from: slice}` (bare relation) |
| flt1 | WHERE(1) | FROM(0) | folds |
| ext | SELECT(3) | WHERE(1) | folds |
| gb | GROUP BY(2) + SELECT(3) | SELECT(3) | **no fold**: `ext` becomes CTE `extend_1` |

```sql
WITH
extend_1 AS (
  SELECT
    *,
    dur / 1e6 AS dur_ms
  FROM slice
  WHERE dur > 1000
)
SELECT
  name,
  COUNT(*) AS cnt,
  SUM(dur_ms) AS total_ms
FROM extend_1
GROUP BY name
```

Step `_qb_171cdcd6`: `gb` is outside the step, so it's read from its table, and
filter and sort fold together:

```sql
SELECT *
FROM _qb_324566e1
WHERE cnt > 5
ORDER BY total_ms DESC
```

Output: always Standard SQL; the limit folds onto a read of the sort table:

```sql
SELECT *
FROM _qb_171cdcd6
LIMIT 10
```

Other folding behaviour to know:
- **A clause that comes earlier never folds.** `limit → filter`,
  `extend → filter` and `groupby → filter` always start a new block. A filter
  after a projection may refer to its aliases, and a filter after a limit means
  something different.
- **Inputs referred to by name become CTEs.** When a generator reads `inp.ref`
  (joins, II, union, SQL node), that input is always materialized as a CTE or
  table first. Inputs used more than once within the query become a CTE once
  and are referenced by every consumer (see the shared-node example).
- **Nested `WITH` is possible.** Nodes whose fragment is a whole subquery
  (interval ops, union, data_table) render as `SELECT * FROM (…)`, possibly with
  their own `WITH` nested inside.

### PerfettoSQL Next: stage appending

PSN needs no folding, because pipe stages already map one-to-one onto nodes:
- **A `stages` fragment** is appended to the primary input's pipeline.
- **A `source` fragment** starts a new pipeline.
- **An input used more than once** is **repeated**, since PSN has no CTEs.
- **Sub-pipelines used as sources** are always written multi-line in
  parentheses. A bare table collapses to its name, and a plain SQL subquery
  source is used as-is.

Step `_qb_ba3e9372` (PSN):

```
FROM slice
|> WHERE dur > 1000
|> EXTEND dur / 1e6 AS dur_ms
|> AGGREGATE COUNT(*) AS cnt, SUM(dur_ms) AS total_ms GROUP BY name
```

Step `_qb_f5c00c00` (PSN):

```
FROM _qb_ba3e9372
|> WHERE cnt > 5
|> ORDER BY total_ms DESC
```

### The SQL tab (display query)

`buildDisplayQuery(plan, dialect)` composes the **whole plan as one query**,
with no cache tables. That's the query you'd run by hand.

Standard SQL:

```sql
WITH
extend_1 AS (
  SELECT
    *,
    dur / 1e6 AS dur_ms
  FROM slice
  WHERE dur > 1000
),
groupby_1 AS (
  SELECT
    name,
    COUNT(*) AS cnt,
    SUM(dur_ms) AS total_ms
  FROM extend_1
  GROUP BY name
)
SELECT *
FROM groupby_1
WHERE cnt > 5
ORDER BY total_ms DESC
LIMIT 10
```

PerfettoSQL Next:

```
FROM slice
|> WHERE dur > 1000
|> EXTEND dur / 1e6 AS dur_ms
|> AGGREGATE COUNT(*) AS cnt, SUM(dur_ms) AS total_ms GROUP BY name
|> WHERE cnt > 5
|> ORDER BY total_ms DESC
|> LIMIT 10
```

---

## 4. Executor and cache (`MaterializationService`)

The cache is a map from hash to cache entry. The table for hash `h` in
TraceProcessor is named `h` (e.g. `_qb_324566e1`).

**Each run:**
1. **Plan:** `buildPlan`, then `planMaterialization(plan, dialect)`. It's skipped
   if the display query and dialect haven't changed since the last run.
2. **Work out the needed steps** (`computeNeededStepHashes`): walk back from the
   output's deps through the step deps, **stopping at any step that's already
   cached**. Ancestors of a cached step aren't needed, even if they've been
   evicted.
3. **Run the includes:** one `INCLUDE PERFETTO MODULE` per module in the plan.
4. **Run the needed steps in dependency order:**
   - **Already cached:** a hit. Update the hit count and last-hit time; no SQL
     runs.
   - **Not cached:** run
     `DROP TABLE IF EXISTS h; CREATE PERFETTO TABLE h AS <formatStep body>`,
     then record a cache entry holding that body (the text `h` was computed
     from) and how long it took.
   - **Not needed at all:** marked skipped.
5. **Output:** run `formatOutput` and hand it to the DataGrid's
   `SQLDataSource`. If the output is just a table, it's read directly as
   `SELECT * FROM <table>`.

**Run outcomes** for each step: `building` → `built` (with time) / `hit` /
`skipped`. The Materialization tab shows these as badges.

**Eviction (least recently used):**
- All cache tables share one cap, `maxEntries` (default 16). After each run
  (never mid-run), anything over the cap is dropped, oldest last hit first.
- The tables the current output query reads are never evicted.
- Re-running a plan only needs its most downstream cached step, so dropping
  older upstream tables never causes a rebuild cascade, even when a plan has
  more steps than the cap.
- Drops wait for in-flight DataGrid queries on the shared task queue.

---

## Edit scenarios: what the cache reuses

Starting from the example above in Standard SQL, with both steps cached
(`warm = {_qb_324566e1, _qb_171cdcd6}`). These are real results:

| Edit | Step hashes that change | Needed steps | What runs |
|---|---|---|---|
| **B:** Limit 10 → 20 | none (the limit is in the output) | `_qb_171cdcd6` (cached) | No `CREATE`; only the output query changes (`LIMIT 20`) |
| **C:** cnt > 5 → 50 | sort step only (`_qb_171cdcd6` → `_qb_547fdb70`) | `_qb_547fdb70` (new), `_qb_324566e1` (cached) | One `CREATE` (the sort step, reading the cached groupby table) |
| **D:** dur > 1000 → 5000 | both (`_qb_52e94ac5`, `_qb_e7f5e0ec`) | both (new) | Both steps are rebuilt |
| **E:** select `flt2` instead | none (subset of the plan) | `_qb_324566e1` (cached) | No `CREATE`; output is `SELECT * FROM _qb_324566e1 WHERE cnt > 5` |
| **G:** switch to PSN | all (`_qb_ba3e9372`, `_qb_f5c00c00`) | both (new) | Both steps are rebuilt as PSN tables |

In C:

```
  step _qb_324566e1 root=gb  nodes=[src,flt1,ext,gb] deps=[]              ← cached hit
  step _qb_547fdb70 root=srt nodes=[flt2,srt]        deps=[_qb_324566e1]  ← built
```

```sql
-- _qb_547fdb70
SELECT *
FROM _qb_324566e1
WHERE cnt > 50
ORDER BY total_ms DESC
```

Notes on these results:
- **Editing a cheap node after the last expensive one** (B) never builds a
  table; only the output query changes.
- **Reverting an edit** (e.g. cnt back to 5) gets an instant hit on the old
  table, if it hasn't been evicted, because it renders the same SQL.
- **Selecting a different node** (E) reuses every table upstream of it. The
  cheap nodes after the last table run in the output query on every DataGrid
  page fetch, which is the trade-off of not caching cheap nodes.
- **Switching dialect** (G) starts from a cold cache for that dialect. Switching
  back reuses the other dialect's tables if they haven't been evicted.

---

## A shared node: one CTE (SQL) vs repetition (PSN)

`From(slice) → Filter(dur > 0)` feeding **both** inputs of a Union:

```
  src refCount=1
  flt refCount=2     ← used twice
  uni refCount=0     (union is expensive → one step)
```

Standard SQL (`_qb_ea58b357`): the shared filter is written once as a CTE.

```sql
WITH
filter_1 AS (
  SELECT *
  FROM slice
  WHERE dur > 0
)
SELECT *
FROM (
  SELECT * FROM filter_1
  UNION ALL
  SELECT * FROM filter_1
)
```

PerfettoSQL Next (`_qb_322fd832`): no CTEs, so the pipeline is repeated. The
union is a pipe stage on the first input's pipeline. This uses the proposed
`|> UNION` syntax, which TraceProcessor doesn't support yet.

```
FROM slice
|> WHERE dur > 0
|> UNION ALL (
  FROM slice
  |> WHERE dur > 0
)
```

> [!NOTE]
> Within a query, sharing is by **node id**. Two separate but identical branches
> (different nodes) become two CTEs (`filter_1`, `filter_2`). If they're
> expensive, they still share a single *cache table*, because their steps render
> identical SQL and so get the same hash.

---

## Where things live

| Concern | File |
|---|---|
| Fragment types, `NodeGenerators`, `expensive` flag | [node_types.ts](./node_types.ts) |
| Per-node SQL / PSN generators | `nodes/*.ts` |
| Shared SQL helpers and types | [sql_types.ts](./sql_types.ts), [generator_utils.ts](./generator_utils.ts) |
| Graph → Plan (structure, passthroughs, incomplete nodes) | [plan.ts](./plan.ts) |
| Folding (SQL), stage appending (PSN), display query | [compose.ts](./compose.ts) |
| Steps, step hashes, `formatStep` / `formatOutput`, executor, cache, eviction | [materialization.ts](./materialization.ts) |
| Runtime checks / headless evaluation (temp tables via `tableName`) | [graph_check.ts](./graph_check.ts), [node_eval.ts](./node_eval.ts) |
