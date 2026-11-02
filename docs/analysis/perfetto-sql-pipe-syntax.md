# PerfettoSQL Pipe Syntax Reference

> **Warning:** Pipe syntax is **experimental and unstable**. Do **not** rely on
> it in dashboards, scripts, or production queries. Anything can and will break
> without notice. Outside the PerfettoSQL standard library, enable it on a
> connection with `PERFETTO PRAGMA pipelines = 1;`.

Related pages:

- [Getting Started with PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-getting-started.md)
- [Migrating to PerfettoSQL Pipe Syntax](/docs/analysis/perfetto-sql-pipe-migration.md)
- [PerfettoSQL Pipe Operator Implementation Notes](/docs/design-docs/perfetto-sql-pipelines.md)

---

## Pragma and Statement Forms

### `PERFETTO PRAGMA pipelines` {#perfetto-pragma-pipelines}

Turns pipe syntax on or off for the connection. The standard library can always
use it.

```sql
PERFETTO PRAGMA pipelines = 1;  -- On
PERFETTO PRAGMA pipelines = 0;  -- Off
```

The value must be a non-negative integer literal; any value other than `0`
turns pipelines on. The pragma name is case-sensitive.

### Top-level forms

A pipeline can be a statement on its own, or the body of a `PERFETTO TABLE`:

```sql
<pipeline>;

CREATE [OR REPLACE] PERFETTO TABLE <table_name> [(<schema>)] AS
<pipeline>;
```

It cannot yet be the body of a view, function or `WITH` clause, or appear inside
a subquery.

A `<pipeline>` is a source followed by any number of stages:

```railroad pipeline
Diagram(
  NonTerminal('source-clause'),
  ZeroOrMore(Sequence(Terminal('|>'), NonTerminal('stage'))))
```

- `|>` must be written with no space between `|` and `>`.
- `TREE`, `ACCUMULATE`, `UP`, `DOWN`, `INTERVAL`, `INTERSECTION`, `FLATTEN`,
  `FILL`, `GAPS`, `AGGREGATE`, `PER`, `EXTEND`, `GRAPH`, `BFS`, `DFS`,
  `DOMINATOR`, `NODES` and `EDGES` are not reserved, so they can still be used
  as names in SQL.
- An alias always needs `AS`: `FROM slice AS s`, not `FROM slice s`.

A `<source-clause>` is [`FROM`](#from),
[`INTERVAL INTERSECTION OF`](#interval-intersection-of) or
[`GRAPH ... TREE`](#graph-tree). A `<stage>` is one of:

```railroad stage
Diagram(Choice(0,
  Link(Terminal('WHERE'), '#where'),
  Link(Terminal('DISTINCT'), '#distinct'),
  Link(Terminal('ORDER BY'), '#order-by'),
  Link(Terminal('LIMIT'), '#limit'),
  Link(Terminal('INTERVAL FLATTEN'), '#interval-flatten'),
  Link(Terminal('INTERVAL FILL GAPS'), '#interval-fill-gaps'),
  Link(Terminal('TREE ACCUMULATE'), '#tree-accumulate'),
  Link(Terminal('AGGREGATE'), '#aggregate'),
  Link(Terminal('SELECT'), '#select'),
  Link(Terminal('EXTEND'), '#extend'),
  Link(Terminal('DROP'), '#drop'),
  Link(Terminal('RENAME'), '#rename'),
  Link(Terminal('SET'), '#set'),
  Link(Terminal('AS'), '#as')))
```

### Shared rules

A `<source>` is a table or a subquery, with an optional alias:

```railroad source
Diagram(
  Choice(0,
    NonTerminal('table-name'),
    Sequence(Terminal('('), NonTerminal('select-stmt'), Terminal(')'))),
  Optional(Sequence(Terminal('AS'), NonTerminal('alias'))))
```

A `<column>` can be qualified by an alias:

```railroad column
Diagram(Optional(Sequence(NonTerminal('alias'), Terminal('.'))), NonTerminal('name'))
```

A `<per-clause>` lists the columns that split rows into groups:

```railroad per-clause
Diagram(Terminal('PER'), OneOrMore(NonTerminal('column'), Terminal(',')))
```

A column listed again groups nothing new.

### Limits

- A pipeline outputs at most 256 columns.
- Every column of a source must have a distinct name made of letters, digits
  and `_`, not starting with a digit. This holds even for columns a later stage
  drops:
  - A column with no name, such as `SELECT 1 + 1`, fails with `expected every
    column to have a name, but column <n> has none: give it one with AS`.
  - A name which is not a valid identifier fails with `expected every column
    to have a valid name, but '<name>' is not one: give it one with AS`.
  - Two columns with one name fail with `expected distinct column names, but
    there are two named '<name>'`.

---

## Sources

### `FROM` {#from}

```railroad
Diagram(Terminal('FROM'), NonTerminal('source'))
```

- `<table-name>` is a table, a view, or a macro which expands to one. A
  schema-qualified name such as `main.t` is not supported.
- `<select-stmt>` is any SQL `SELECT`.
- **Qualifiers:** `FROM slice` lets columns be written `slice.<col>`.
  `FROM slice AS s` makes that `s.<col>` instead. An unaliased subquery has no
  qualifier until a later `|> AS <alias>`.

### `INTERVAL INTERSECTION OF` {#interval-intersection-of}

Finds where intervals from two or more sources overlap:

```railroad
Diagram(
  Terminal('INTERVAL'), Terminal('INTERSECTION'), Terminal('OF'),
  Terminal('('), OneOrMore(NonTerminal('source'), Terminal(',')), Terminal(')'),
  Optional(NonTerminal('per-clause')))
```

#### Requirements

- At least two sources.
- Each source has its own alias. A table name is its own alias; a subquery
  needs `AS <alias>`.
- Every source has `ts` and `dur`, and every `PER` column.

#### Output

One row is produced for each region `[ts, ts + dur)` where a row of every
source overlaps, among rows that agree on the `PER` columns.

| Column | Found by bare name | Found through alias |
| :--- | :--- | :--- |
| Region `ts`, `dur` | Yes | No |
| Each source's own `ts`, `dur` | No | `<alias>.ts` |
| `PER` columns of the first source | Yes | Yes |
| `PER` columns of later sources | No | Yes |
| Every other source column | Yes, if only one source has it | Yes |

A bare name two sources share is ambiguous, and an error when something refers
to it.

`*`, `|> AS <alias>` and the result of a pipeline which ends at the
intersection all hold the columns found by bare name: the region's `ts` and
`dur`, then each source's columns in order. When two sources share a column
name, both copies are in the result, which then cannot be saved as a table. Add
a `|> SELECT` naming them through their aliases.

#### Interval rules

- **Half-open:** `[0, 10)` and `[10, 20)` do not overlap.
- **Points:** A row with `dur = 0` is the instant `ts`. It overlaps `[a, b)`
  when `a <= ts < b`, and another point at the same instant. The region has
  `dur = 0`.
- **`NULL` bounds:** A row with a `NULL` `ts` or `dur` is skipped.
- **Negative bounds:** A negative `ts` or `dur` fails with `ts or dur is below
  zero`.
- **Overflow:** `ts + dur` beyond the 64-bit range fails with `integer
  overflow`.
- **`NULL` in `PER` columns:** Rows holding `NULL` in a `PER` column agree on
  it, as in `GROUP BY`.

### `GRAPH ... TREE` {#graph-tree}

Reduces a graph to a tree over the nodes reachable from a set of seeds:

```railroad
Diagram(
  Terminal('GRAPH'), Choice(0, Terminal('BFS'), Terminal('DFS'),
    Terminal('DOMINATOR')),
  Terminal('TREE'),
  Terminal('NODES'), NonTerminal('source'),
  Terminal('EDGES'), NonTerminal('source'),
  Terminal('FROM'), NonTerminal('source'))
```

```sql
GRAPH DOMINATOR TREE
  NODES heap_graph_object
  EDGES (SELECT owner_id AS src, owned_id AS dst FROM heap_graph_reference)
  FROM (SELECT id FROM heap_graph_object WHERE root_type IS NOT NULL)
|> TREE ACCUMULATE UP SUM(self_size) AS retained_size
```

#### Requirements

- `NODES` has an `id` column, `EDGES` has `src` and `dst` columns, and `FROM`
  has an `id` column. All of them hold integers.
- No two nodes have the same `id`, and every node has one.

#### Output

One row for each node reachable from the seeds, with every `NODES` column and
`parent_id`, the `id` of the node's parent in the tree. `parent_id` is `NULL`
for a root. A `NODES` column named `parent_id` is not carried. Parents come
before their children. `NODES` can be given an alias, which qualifies its
columns.

| Kind | Roots | A node's parent |
| :--- | :--- | :--- |
| `BFS` | Every seed | The first node searched with an edge to it |
| `DFS` | Each seed not reached from an earlier one | The node it was reached from |
| `DOMINATOR` | Every seed | Its immediate dominator |

#### Graph rules

- **Order:** Seeds are searched, and each node's edges followed, in the order
  their rows arrive. Where a node can be reached from several equally good
  parents, that order picks one, so sort `FROM` or `EDGES` to choose.
- **Missing nodes:** An edge or seed whose `id` is not a node, or is `NULL`,
  is ignored. `NODES` can be a subset of the graph `EDGES` describes.
- **Dominators:** A node's dominators are the nodes every path to it from the
  seeds passes through, as though one more node had an edge to every seed.

---

## Aggregates

`INTERVAL FLATTEN`, `TREE ACCUMULATE` and `AGGREGATE` take an
`<aggregate-list>`, and treat every aggregate the same way:

```railroad aggregate-list
Diagram(OneOrMore(
  Sequence(NonTerminal('aggregate'), Terminal('AS'), NonTerminal('name')),
  Terminal(',')))
```

```railroad aggregate
Diagram(Choice(0,
  Sequence(Terminal('COUNT'), Terminal('('), Terminal('*'), Terminal(')')),
  Sequence(
    Choice(0, Terminal('SUM'), Terminal('MIN'), Terminal('MAX')),
    Terminal('('), NonTerminal('column'), Terminal(')'))))
```

- **Forms:** Each aggregate needs an `AS` name. Expressions, `DISTINCT`,
  `FILTER`, `OVER` and other functions are rejected.
- **`COUNT(*)`** counts rows.
- **`SUM`, `MIN` and `MAX`** take an integer column. They skip `NULL`s and
  return `NULL` if every value is `NULL`. A column from a SQL source whose type
  is not known in advance is checked when the query runs.
- **Results** are `LONG` columns. Overflow is an error.
- **Scope:** Every aggregate reads the row from before the stage, so one
  aggregate cannot read another.
- **Names:** An aggregate can take a name another column has. That name is then
  ambiguous, and an error when a later stage refers to it.

---

## Stages

### `WHERE` {#where}

Keeps the rows that match a condition:

```railroad
Diagram(
  Terminal('WHERE'),
  OneOrMore(
    Sequence(
      NonTerminal('column'),
      Choice(0,
        Sequence(NonTerminal('op'), NonTerminal('literal')),
        Sequence(Terminal('GLOB'), NonTerminal('string')),
        Sequence(Terminal('IN'), Terminal('('),
                 OneOrMore(NonTerminal('literal'), Terminal(',')),
                 Terminal(')')),
        Sequence(Terminal('BETWEEN'), NonTerminal('literal'), Terminal('AND'),
                 NonTerminal('literal')),
        Sequence(Terminal('IS'), Optional(Terminal('NOT')), Terminal('NULL')))),
    Terminal('AND')))
```

- `<op>` is one of `=`, `==`, `!=`, `<>`, `<`, `<=`, `>` and `>=`.
- `<literal>` is an integer, floating-point or string literal.
- `GLOB` matches only string columns.
- `NULL` never matches a comparison, `GLOB` or `IN`. Integers compare with
  fractions as in SQL, and numbers sort before strings. A string is never
  converted to a number, so `dur = '5'` matches nothing, as in a filter on a
  Perfetto table.
- Keeps all columns and aliases.
- Not supported yet: `OR`, `NOT`, `NOT IN`, `NOT GLOB`, `LIKE`, `REGEXP`,
  parentheses, functions, arithmetic, and a literal on the left. Filter in the
  source subquery instead.

### `DISTINCT` {#distinct}

Removes repeated rows:

```railroad
Diagram(Terminal('DISTINCT'))
```

- Rows are equal when they agree on every column found by bare name. `NULL`s
  are equal.
- Columns only an alias finds, such as an intersection's `x.ts`, are not
  compared, and are dropped since they have no one value per row. Aliases are
  kept, holding only the columns that were compared.
- The first of each set of equal rows is kept, and kept rows stay in order.
- A row can have at most 64 columns.

### `ORDER BY` {#order-by}

Sorts the rows:

```railroad
Diagram(
  Terminal('ORDER'), Terminal('BY'),
  OneOrMore(
    Sequence(NonTerminal('column'),
      Optional(Choice(0, Terminal('ASC'), Terminal('DESC')))),
    Terminal(',')))
```

- The first key decides the order and each later key breaks ties. Each defaults
  to `ASC`.
- A key is a column, which can be qualified. Expressions are not supported yet.
- Strings sort by their bytes, as SQLite's default `BINARY` collation does.
- `NULL`s come first when ascending and last when descending, as in SQLite.
- Rows equal on every key keep their order.
- Keeps all columns and aliases.
- `INTERVAL FLATTEN`, `INTERVAL FILL GAPS`, `TREE ACCUMULATE` and `AGGREGATE`
  arrange rows their own way, so put `ORDER BY` after them.

### `LIMIT` {#limit}

Keeps the first rows, after skipping some:

```railroad
Diagram(
  Terminal('LIMIT'), NonTerminal('count'),
  Optional(Sequence(Terminal('OFFSET'), NonTerminal('skipped'))))
```

- Skips `<skipped>` rows, then keeps up to `<count>` rows, in the order they
  arrive. Put it after [`ORDER BY`](#order-by) to keep the first rows of a
  sort.
- `<count>` and `<skipped>` are integer literals. As in SQL, a negative count
  keeps every row and a negative offset skips none.
- Keeps all columns and aliases.

### `INTERVAL FLATTEN` {#interval-flatten}

Splits overlapping intervals into segments and aggregates the rows active in
each:

```railroad
Diagram(
  Terminal('INTERVAL'), Terminal('FLATTEN'), Optional(NonTerminal('per-clause')),
  Terminal('AGGREGATE'), NonTerminal('aggregate-list'))
```

- **Intervals:** Bare `ts` and `dur`, which must be integer columns. Input does
  not need to be sorted.
- **Groups:** Each combination of `PER` values is flattened on its own. Without
  `PER`, all rows are one group. `NULL` keys group together.
- **Aggregates:** At least one; see [Aggregates](#aggregates). Each is taken
  over the rows covering a segment.
- **Output:** `ts`, `dur`, the `PER` columns, then the aggregates. Every other
  column and all aliases are dropped. A `PER` column listed twice is output
  twice, so its name is then ambiguous.

Interval rules:

- **Segments:** Output splits at every input start and end. Gaps with no rows
  give no output. Neighbouring segments are not merged even when their values
  match.
- **Points:** Rows with `dur = 0` at one instant give one extra segment with
  `dur = 0`. Its aggregates cover those points and the intervals active at that
  instant: intervals starting there count, intervals ending there do not.
  Points do not count towards other segments.
- **`NULL` bounds:** Rows with a `NULL` `ts` or `dur` are skipped.
- **Negative bounds** are an error. Filter out unfinished slices, which have
  `dur = -1`, first.
- **Overflow** is an error.

For an example, see
[Flattening Overlapping Intervals](/docs/analysis/perfetto-sql-pipe-getting-started.md#flattening-overlapping-intervals-interval-flatten).

### `INTERVAL FILL GAPS` {#interval-fill-gaps}

Keeps every row and adds a row for each stretch of a background interval which
no row covers:

```railroad
Diagram(
  Terminal('INTERVAL'), Terminal('FILL'), Terminal('GAPS'), Terminal('WITH'),
  NonTerminal('source'), Optional(NonTerminal('per-clause')))
```

`<source>` holds the background intervals.

- **Intervals:** Bare `ts` and `dur`, in the input and the background.
- **Lanes:** Each combination of `PER` values is filled on its own. The
  background has either every `PER` column or none of them:
  - With them, it says which lanes exist. A lane with no input is filled whole,
    and input in a lane the background does not have gets no fillers.
  - Without them, the same background applies to every lane in the input.
- **Fillers** take each background column's value by name, their lane's `PER`
  values, and `NULL` for every other column. Every background column must also
  be in the input.
- **Types:** A column takes one type across both inputs. A column holding
  strings in one and numbers in the other is an error.
- **Output:** The input's columns. Rows come out in no particular order. Aliases
  are kept.

Interval rules:

- Overlapping input rows cover their union.
- An input row with `dur = -1` covers everything after its `ts`.
- An input row with a `NULL` `ts` covers nothing, but is still output.
- A point does not split a filler.
- A negative input `ts` is an error.
- Background rows which overlap within a lane are an error.

For an example, see
[Filling Gaps](/docs/analysis/perfetto-sql-pipe-getting-started.md#filling-gaps-interval-fill-gaps).

### `TREE ACCUMULATE` {#tree-accumulate}

Aggregates up or down a tree:

```railroad
Diagram(
  Terminal('TREE'), Terminal('ACCUMULATE'),
  Choice(0, Terminal('UP'), Terminal('DOWN')),
  NonTerminal('aggregate-list'))
```

- **Tree:** Bare `id` and `parent_id`. Roots have `parent_id IS NULL`.
- **Directions:**
  - `UP`: each node's aggregate covers itself and all its descendants. Rows
    come out children first.
  - `DOWN`: each node's aggregate covers itself and all its ancestors. Rows
    come out parents first.
- **Aggregates:** See [Aggregates](#aggregates). `COUNT(*)` going `DOWN` is a
  node's depth, counting from 1.
- **Output:** Every input column, then the aggregates. Aliases are kept but do
  not include the new columns.
- **Errors:** Two rows with one `id`, a row which is its own parent, and a
  `parent_id` with no row of that `id`. These are only checked if a later stage
  reads one of the aggregates; otherwise the stage is skipped.

### `AGGREGATE` {#aggregate}

Collapses each group of rows into one:

```railroad
Diagram(
  Terminal('AGGREGATE'), NonTerminal('aggregate-list'),
  Optional(NonTerminal('group-by')))
```

```railroad group-by
Diagram(
  Terminal('GROUP'), Terminal('BY'),
  OneOrMore(
    Sequence(NonTerminal('column'),
      Optional(Sequence(Terminal('AS'), NonTerminal('name')))),
    Terminal(',')))
```

- **Groups:** Rows that agree on every `GROUP BY` column. A key can have any
  type and can be qualified. `NULL` keys group together. Without `GROUP BY`,
  all rows are one group, which gives a row even when there are no rows.
- **Aggregates:** At least one; see [Aggregates](#aggregates).
- **Output:** The keys, then the aggregates. A key is named by its `AS` name,
  or else its column name without qualifier. Every other column and all aliases
  are dropped. A key listed twice, even as `cpu` and `t.cpu`, groups once
  but is output twice.
- **Order:** Groups come out in the order their first rows arrived.

For example, CPU time per slice:

```sql
INCLUDE PERFETTO MODULE slices.with_context;

INTERVAL INTERSECTION OF (
  (SELECT id, ts, dur, utid FROM thread_slice WHERE dur > 0) AS s,
  (SELECT ts, dur, utid FROM sched WHERE dur > 0) AS running
) PER utid
|> AGGREGATE SUM(dur) AS cpu_time GROUP BY s.id
|> ORDER BY id;
```

### `SELECT` {#select}

Replaces the row with the listed items, and clears all aliases:

```railroad
Diagram(Terminal('SELECT'), OneOrMore(NonTerminal('item'), Terminal(',')))
```

Each `<item>` is one of:

```railroad item
Diagram(Choice(0,
  Sequence(NonTerminal('column'),
    Optional(Sequence(Terminal('AS'), NonTerminal('name')))),
  NonTerminal('star')))
```

- **A column.** Without `AS`, it keeps its name. Other expressions parse, but
  fail with `a computed column is not supported yet`.
- **A `<star>`:**

```railroad star
Diagram(
  Optional(Sequence(NonTerminal('alias'), Terminal('.'))), Terminal('*'),
  Optional(NonTerminal('except')), Optional(NonTerminal('replace')))
```

```railroad except
Diagram(
  Terminal('EXCEPT'), Terminal('('),
  OneOrMore(NonTerminal('name'), Terminal(',')), Terminal(')'))
```

```railroad replace
Diagram(
  Terminal('REPLACE'), Terminal('('),
  OneOrMore(
    Sequence(NonTerminal('column'), Terminal('AS'), NonTerminal('target')),
    Terminal(',')),
  Terminal(')'))
```

A star works as follows:

1. `*` is the columns found by bare name. After `INTERVAL INTERSECTION OF`,
   that leaves out each source's own `ts` and `dur`. `<alias>.*` is the
   columns the alias had when it was set.
2. `EXCEPT` removes every column with a listed name. Each name must match at
   least one column and be listed once, and at least one column must remain.
3. `REPLACE (<column> AS <target>)` puts `<column>`'s values in `<target>`,
   keeping `<target>`'s name and position. `<column>` is looked up in the row
   before the stage; `<target>` must be one column left after `EXCEPT`.
4. Every column a star adds can be referred to by bare name in later stages.

### `EXTEND` {#extend}

Adds columns at the end of the row, keeping every column and alias:

```railroad
Diagram(Terminal('EXTEND'), OneOrMore(NonTerminal('item'), Terminal(',')))
```

- Items are as in `SELECT`, except that a bare `*` is not allowed. Use
  `<alias>.*` to add an alias's columns.
- Items read the row before the stage, so one cannot refer to another.
- Aliases do not include the new columns.

### `DROP` {#drop}

Removes columns by name:

```railroad
Diagram(Terminal('DROP'), OneOrMore(NonTerminal('name'), Terminal(',')))
```

- Each name must match a column found by bare name, and removes every column
  of that name.
- Each name is listed once, and at least one column must remain.
- Aliases keep the dropped columns. An alias with the same name as a dropped
  column is removed.

### `RENAME` {#rename}

Renames columns in place:

```railroad
Diagram(
  Terminal('RENAME'),
  OneOrMore(
    Sequence(NonTerminal('old'), Terminal('AS'), NonTerminal('new')),
    Terminal(',')))
```

- Each `<old>` must be one column found by bare name, listed once.
- All renames happen together, so `|> RENAME x AS y, y AS x` swaps `x` and
  `y`.
- Renaming to an existing name is allowed; the name is then ambiguous.
- Aliases keep the old name.

### `SET` {#set}

Replaces the values of columns in place:

```railroad
Diagram(
  Terminal('SET'),
  OneOrMore(
    Sequence(NonTerminal('target'), Terminal('='), NonTerminal('column')),
    Terminal(',')))
```

- Each `<target>` must be one column found by bare name, listed once.
- Each `<column>` is looked up in the row before the stage. So `|> SET x = y, y = x` swaps the values of `x` and `y`. Other
  expressions fail with `a computed column is not supported yet`.
- Aliases keep the old values. An alias with the same name as a target is
  removed.

### `AS` {#as}

Replaces all aliases with one alias for the current row:

```railroad
Diagram(Terminal('AS'), NonTerminal('alias'))
```

After `|> AS u`, earlier aliases are gone, and `u.<col>` and `u.*` are the
columns found by bare name at this point. After `INTERVAL INTERSECTION OF`,
`u.ts` is the region's `ts`, and the sources' own `ts` and `dur` can no longer
be reached.

---

## Names

- **Case:** Column names and aliases match regardless of ASCII case. Output
  keeps the case they were defined with.
- **Duplicates:** A stage can leave two columns with one name, as
  `|> RENAME x AS y` does when `y` exists. This is only an error when a later
  stage refers to that name without a qualifier. `DROP y` or `* EXCEPT (y)`
  removes every copy.
- **Quoting:** Names can be quoted with `"..."`, `` `...` `` or `[...]`. Inside
  `"..."` and `` `...` ``, a doubled quote is a literal quote.
