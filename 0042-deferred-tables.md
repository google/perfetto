# Deferred tables

**Authors:** @LalitMaganti

**Status:** Draft

## Problem

PerfettoSQL modules often expose tables whose contents are expensive to compute.
`CREATE PERFETTO TABLE` computes them when the module is included, even if the
query never uses them. A view avoids that upfront cost, but repeats the work on
every access and does not support table features such as indexes.

We want modules to expose relations which behave like tables without requiring
every table to be computed when the module is included.

## Decision

Pending

## Design

Add `CREATE PERFETTO DEFERRED TABLE`:

```sql
CREATE PERFETTO DEFERRED TABLE expensive_result AS
SELECT ...;
```

A deferred table has the same query-visible semantics as a Perfetto table.
Perfetto may defer computing and storing its contents until the contents are
needed. The exact point of computation is an implementation detail.

When used inside a PerfettoSQL module, the defining query is prepared when the
module is included. This validates the query and determines its schema in the
same way as existing PerfettoSQL statements, but does not require its rows to be
computed. Any operation which needs the rows causes the table to be computed.
In particular, `CREATE PERFETTO INDEX` on a deferred table computes the table
immediately before creating the index.

Outside a PerfettoSQL module, a deferred table is computed eagerly. This makes
module SQL safe to copy into an interactive or top-level query without
introducing deferred failures or other surprising state.

Computation is atomic: a caller observes either the complete table or an error,
never a partial result. If computation fails, the deferred table remains failed
and subsequent accesses fail without retrying. Direct or indirect recursive
computation of a deferred table also fails.

Perfetto may evict a computed deferred table and later recompute it. Indexes are
recreated with the table. A deferred table's defining query must therefore be
deterministic; using nondeterministic SQL in its definition is not supported.
Trace Processor data is read-only, so deferred tables do not need refresh or
invalidation semantics for mutations.

The initial implementation will use a virtual table which computes and stores
its backing table when required.

## Alternatives considered

### Materialized views

`CREATE PERFETTO MATERIALIZED VIEW` uses familiar terminology, but materialized
views in other databases are generally already materialized and require refresh
semantics. Neither implication applies here.

### Lazy tables

`CREATE PERFETTO LAZY TABLE` emphasizes the expected implementation rather than
the contract. "Deferred" permits eager computation where appropriate and allows
the implementation to change when computation happens.

### Views

Views avoid work when unused, but recompute their query on each access and
cannot be indexed like Perfetto tables.

### Perfetto tables

Existing Perfetto tables have the desired behavior after creation, but eagerly
computing every exported module table makes module inclusion unnecessarily
expensive.

## Open questions

* Should accesses to a failed deferred table reproduce the original error or
  report that its earlier computation failed?
