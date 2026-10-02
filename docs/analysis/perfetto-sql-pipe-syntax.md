# PerfettoSQL Pipe Syntax Reference

This page is the language syntax and semantics reference for PerfettoSQL
pipelines.

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

### `PERFETTO PRAGMA pipelines`

Controls whether pipe syntax is permitted on the current connection outside the
standard library:

```sql
PERFETTO PRAGMA pipelines = 1;  -- Enable pipelines
PERFETTO PRAGMA pipelines = 0;  -- Disable pipelines
```

The right-hand side must evaluate to a whole number. Unknown pragma names
produce an error.

### Top-level forms

A pipeline is valid in two statement positions:

```sql
-- 1. Standalone query
<pipeline>;

-- 2. Materialized Perfetto table
CREATE [OR REPLACE] PERFETTO TABLE <table_name> [(<schema>)] AS
<pipeline>;
```

A `<pipeline>` consists of a source, either `FROM` or `INTERVAL INTERSECTION OF`,
followed by zero or more `|>` stages:

```sql
<source>
[|> <stage_1>]
[|> <stage_2>]
...
```

> **Note:** The pipe operator `|>` must be written with no whitespace between
> `|` and `>`. The pipeline keywords `TREE`, `ACCUMULATE`, `UP`, `DOWN`,
> `INTERVAL`, `INTERSECTION`, `PER`, and `EXTEND` are non-reserved and remain
> valid as normal identifiers in standard SQL statements.

---

## Pipeline Sources

### `FROM`

Starts a pipeline from a single relation:

```sql
FROM [<schema>.]<table_name> [[AS] <alias>]
FROM (<select_stmt>) [[AS] <alias>]
```

- `<table_name>` may be any table or view, or a macro invocation that expands to
  a table or parenthesized subquery.
- `(<select_stmt>)` may be any valid SQL `SELECT` statement enclosed in
  parentheses.
- **Table qualifier:**
  - If `<alias>` is given, columns of the source can be qualified with
    `<alias>.<col>`. When the source is a table name, `<alias>` replaces
    `<table_name>` as the qualifier.
  - If the source is an unaliased `<table_name>`, columns can be qualified with
    `<table_name>.<col>`.
  - If the source is an unaliased `(<select_stmt>)`, its columns have no table
    qualifier unless a later `|> AS <alias>` stage binds one.

### `INTERVAL INTERSECTION OF`

Starts a pipeline with the overlapping time regions across two or more
relations:

```sql
INTERVAL INTERSECTION OF (
  <source_1> [AS] <alias_1>,
  <source_2> [AS] <alias_2>
  [, <source_N> [AS] <alias_N> ...]
) [PER <col_1> [, <col_2> ...]]
```

#### Requirements

1. At least two operand sources must be listed.
2. Every operand must have a distinct alias. An unaliased table name acts as its
   own alias; a parenthesized subquery must specify `[AS] <alias>`.
3. Every operand must contain `ts` and `dur` columns, and every column listed in
   `PER` must exist in every operand.

#### Output columns and scoping

For each overlapping region `[ts, ts + dur)` shared by all operands within a
matching `PER` key group, one row is produced with the following columns:

1. `ts` and `dur`: 64-bit integer columns holding the intersected region's start
   and duration. These are the only `ts` and `dur` columns visible to
   unqualified column lookups.
2. Each operand's columns in operand order:
   - The first operand's `PER` key columns are visible both unqualified, such as
     `cpu`, and qualified, such as `<alias_1>.cpu`.
   - All other operand columns are qualified-only. This includes each operand's
     original `ts` and `dur`, as well as subsequent operands' copies of `PER`
     columns. They can only be referenced via `<alias_i>.<col>` or expanded via
     `*` or `<alias_i>.*`.

#### Interval semantics

- **Half-open bounds:** Each interval is `[ts, ts + dur)`. Two intervals
  touching at their endpoints, such as `[0, 10)` and `[10, 20)`, do not
  intersect.
- **Zero-duration points:** A row with `dur = 0` is a point at instant `ts`. It
  intersects an interval `[a, b)` when `a <= ts < b`, and intersects another
  point when both sit at the same instant `ts`, producing a region with
  `dur = 0`.
- **Missing bounds:** A row with `ts IS NULL` or `dur IS NULL` covers no time
  and is skipped.
- **Negative bounds:** A row with `ts < 0` or `dur < 0` fails with the error
  `ts or dur is below zero`.
- **`NULL` in `PER` columns:** Two rows with `NULL` in a `PER` column agree on
  that column, matching `GROUP BY` semantics.

### Source column naming rules

Every column of any pipeline source must have a valid identifier name matching
`^[a-zA-Z_][a-zA-Z0-9_]*$` and must be distinct within that source, even if a
later stage drops the column:

- Unaliased SQL expressions like `1 + 1` or `max(ts)` are rejected with:
  `expected every column to have a valid name, but '<expr>' is not one: give it one with AS`.
- Duplicate column names in a source are rejected with:
  `expected distinct column names, but there are two named '<name>'`.

---

## Pipeline Stages

### `TREE ACCUMULATE`

Accumulates sums up or down a tree defined by `id` and `parent_id`:

```sql
|> TREE ACCUMULATE UP|DOWN
  SUM([<qualifier>.]<col_1>) AS <out_1>
  [, SUM([<qualifier>.]<col_2>) AS <out_2> ...]
```

- **Tree structure columns:** Resolves bare `id` and `parent_id` in the current
  row. Root nodes have `parent_id IS NULL`.
- **Directions:**
  - `UP`: Bottom-up fold. Each node's aggregate is the sum of its own value and
    the values of all its descendants. Output rows are emitted child-first.
  - `DOWN`: Top-down fold. Each node's aggregate is the sum of its own value and
    the values of all its ancestors. Output rows are emitted parent-first.
- **Aggregate expressions:**
  - Only `SUM(<column_ref>)` is currently supported. `DISTINCT`, `FILTER`,
    `OVER`, `*`, non-column expressions, and other aggregate functions are
    rejected.
  - The target column must be an integer type: `ID`, `UINT32`, `INT32`, or
    `LONG`. `NULL` values contribute `0`.
  - All aggregates in a single stage resolve their input columns against the row
    before the stage. A stage cannot reference an output column defined in the
    same stage.
- **Output:** Keeps all columns of the input row and existing table aliases, and
  appends `<out_1>, <out_2>, ...` as `LONG` columns.

---

### `SELECT`

Replaces the current row with the specified items and clears all table aliases:

```sql
|> SELECT <select_item> [, <select_item> ...]
```

Each `<select_item>` is one of:

- **Column reference:** `[<qualifier>.]<col> [[AS] <alias>]`
- **Star expansion:**
  `[<qualifier>.]* [EXCEPT (<col_1> [, <col_2> ...])] [REPLACE (<replace_item> [, ...])]`
  where each `<replace_item>` is `[<qualifier>.]<source_col> AS <target_col>`.
  `AS` is required in `REPLACE`.

Star expansion rules:

1. `*` starts with all columns in the current row, including qualified-only
   operand columns after `INTERVAL INTERSECTION OF`. `<qualifier>.*` starts with
   the columns captured when `<qualifier>` was bound.
2. `EXCEPT (<col>, ...)` removes every column matching any listed bare name
   case-insensitively. Every listed name must match at least one column, no name
   may be listed twice, and at least one column must remain after `EXCEPT`.
3. `REPLACE (<source_col> AS <target_col>, ...)` resolves `<source_col>` against
   the input row or aliases before the stage and replaces the value of
   `<target_col>` in the star's expansion while keeping `<target_col>`'s
   position and name. `<target_col>` must match a single column remaining in the
   star after `EXCEPT`.
4. All columns produced by `*` or `<qualifier>.*` can be referenced without a
   qualifier in subsequent stages.

---

### `EXTEND`

Appends columns to the end of the current row while keeping all existing columns
and aliases:

```sql
|> EXTEND <extend_item> [, <extend_item> ...]
```

- Accepts the same items as `SELECT`, except that a bare `*` is not allowed. Use
  `<qualifier>.*` to append all columns of a table alias.
- All items in `EXTEND` resolve against the row before the stage, so an item
  cannot reference another column added in the same `EXTEND` stage.
- Columns added by `EXTEND` are not added to any existing table aliases.

---

### `DROP`

Removes columns from the current row by bare name:

```sql
|> DROP <col_1> [, <col_2> ...]
```

- Every listed name must match at least one unqualified-accessible column in the
  current row, and removes all columns of that name from the bare row.
- No name may be listed twice, and at least one column must remain in the row
  after `DROP`.
- Existing table aliases keep the dropped columns, except that any table alias
  whose name matches a dropped column name is removed.

---

### `RENAME`

Renames columns in the current row in place:

```sql
|> RENAME <old_1> [AS] <new_1> [, <old_2> [AS] <new_2> ...]
```

- `<old_i>` must be a bare column name that resolves unambiguously in the
  current row, and may not be listed twice in the same `RENAME`.
- All renames in a single stage take effect at the same time, so
  `|> RENAME x AS y, y AS x` swaps `x` and `y`.
- Renaming a column to an existing column's name is allowed. This creates
  duplicate column names in the row, which only error if a later stage
  references the ambiguous name.
- Existing table aliases are unaffected and continue to expose the column under
  its old name.

---

### `SET`

Replaces the values of existing columns in the current row in place:

```sql
|> SET <target_1> = [<qualifier>.]<source_1> [, <target_2> = [<qualifier>.]<source_2> ...]
```

- `<target_i>` must be a bare column name that resolves unambiguously in the
  current row, and may not be listed twice in the same `SET`.
- Every `<source_i>` is resolved against the row and aliases before the stage,
  so `|> SET x = y, y = x` swaps the values of `x` and `y`.
- Existing table aliases keep the pre-`SET` values, except that any table alias
  whose name matches `<target_i>` is removed.

---

### `AS`

Replaces all existing table aliases with a single new alias bound to a snapshot
of the current row:

```sql
|> AS <alias>
```

After `|> AS u`, earlier table aliases are no longer in scope, and `u.<col>` and
`u.*` reflect the row as of the `AS` stage.

---

## Name Resolution and Ambiguity Rules

- **Case insensitivity:** Column names and table aliases are matched
  case-insensitively using ASCII folding, while keeping the casing of the
  defining column or alias in output schemas.
- **Duplicate column names:** A stage is allowed to create duplicate column
  names in the row, such as `|> EXTEND x` or `|> RENAME x AS y` when `y` already
  exists. Duplicate names only cause an error if a later stage references the
  ambiguous name without a table qualifier. You can also drop or exclude all
  copies at once via `DROP y` or `* EXCEPT (y)`.
- **Quoted identifiers:** Identifiers may be quoted with `"..."`, `` `...` ``,
  or `[...]`. Inside `"..."` and `` `...` ``, doubling the quote character
  escapes a literal quote, and `""` spells the empty column name.
