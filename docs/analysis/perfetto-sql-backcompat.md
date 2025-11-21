# PerfettoSQL: backwards compatibility

PerfettoSQL tries its hardest to minimize backwards incompatible changes but occasionally they are unavoidable.
In situations where we need to make such changes which we expect to have non-trivial impact, this page
documents:
 - **Date/Version**: the date of this change and the first release of Perfetto with the change
 - **Symptoms**: unexpected behavior or error messages you would see if you are affected by the change
 - **Context**: why we are making the change i.e. why does it have to backwards incompatible?
 - **Migrations**: suggested changes you can make to your PerfettoSQL to not be broken by the changes

## Removal of `stack_id` and `parent_stack_id` columns from slice table

**Date/Version**

2025-11-21/v54.0

**Symptoms**

- An error message of the form `no such column: stack_id` or `no such column: parent_stack_id`
- The `stack_id` and `parent_stack_id` columns disappearing from output of queries with `SELECT *` on the slice table
- Error `no such function: ancestor_slice_by_stack` or `no such function: descendant_slice_by_stack` (these now require `INCLUDE PERFETTO MODULE slices.stack`)
- Queries using hardcoded stack_id values (e.g., `WHERE stack_id = 123456`) return no results or wrong results

**Context**

The `stack_id` and `parent_stack_id` columns that computed hash values based on slice names. There are two reasons for their removal:

1. They used a lot of memory for a feature which was rarely useful.
2. Even when they were used, usually `parent_id` would have been a better choice.

These columns have been removed from the slice table, and the stack-based table functions have been moved to the `slices.stack` stdlib module where they compute stack hashes on-demand rather than storing them.

**Migrations**

**⚠️ IMPORTANT:** The migration helpers use a different hash algorithm (SQLite `hash()` vs `MurmurHash`) and will produce **different stack_id values** than the previous implementation. This means:
- Hardcoded stack_id values in queries or dashboards will **not work**

**Migration helpers:** For API compatibility with existing queries, you can use the migration helpers in the `slices.stack` module:

```sql
INCLUDE PERFETTO MODULE slices.stack;

-- Access stack_id and parent_stack_id columns (computed on-demand with new hash algorithm)
SELECT * FROM slice_with_stack_id WHERE id = 123;

-- Use stack-based ancestor/descendant functions
SELECT * FROM ancestor_slice_by_stack((SELECT stack_id FROM slice_with_stack_id WHERE id = 123));
SELECT * FROM descendant_slice_by_stack((SELECT stack_id FROM slice_with_stack_id WHERE id = 123));
```

**Note:** These helpers compute stack hashes on-demand and may be slower than the previous C++ implementation.

**Do you really need this?**

Consider whether you actually need the stack-based functionality, or if you intended to use the parent-child relationship instead:

- Use `parent_id` to traverse the slice hierarchy
- Use `ancestor_slice(id)` table function for finding all ancestors
- Use `descendant_slice(id)` table function for finding all descendants

For example, instead of:

```sql
select * from ancestor_slice_by_stack((select stack_id from slice where id = 123))
```

you can do:

```sql
select * from ancestor_slice(123)
```

If you need to find slices with the same name pattern, use explicit name matching:

```sql
-- Find slices with the same category and name
select s2.* from slice s1
join slice s2 on s1.category = s2.category and s1.name = s2.name
where s1.id = 123
```


## Change in semantic of `type` column on track tables

**Date/Version**

2024-12-18/v49.0

**Symptoms**

 - The value of the `type` column changing from output of queries which query a `*track` table
 - Missing rows if you have constraints on the `type` column. e.g.
   `SELECT type from track where type = 'process_slice'` will now return zero rows

**Context**

NOTE: this change is very closely tied to *Removal of `type` column from non-track tables* change,
see below.

The `type` columns on track tables has been around for a long time and indicated the "most specific
table" containing the track. Over time, with changes in how tables in trace processor tables are
structured (i.e. more use of the standard library, tracks with multiple dimensions), we have outgrown
the idea of "object-oriented tables" which made the `type` column meaningful.

Instead of the handful of possible `type` values (e.g. `process_track`, `thread_track`, `counter_track`)
we have switched the semantic of the `type` column to indicate the "type of data in the track". For
example, for global scoped slice tracks coming from `track_event` APIs, the `type` column would now be
`global_track_event`. For process scoped tracks, it would be `process_track_event` etc.

This change very closely ties to the new column `dimension_arg_set_id` which also contains `type` specific
context identifying what makes the track distinct among all tracks of the same `type`.

**Migrations**

If you were doing queries of the form `select * from track where type = 'process_track'`, this can easily
be changed to `select * from process_track`.

Instead if you were trying to export the value of `type` out of trace_processor, you can recover the old
type column by doing multiple unions on track.

For example, instead of:

```sql
select name, type from track where name in ('process_track', 'thread_track')
```

you can do:

```sql
select name, 'process_track' as type from process_track
union all
select name, 'thread_track' as type from thread_track
```

Finally, the suggested way of find all "globally scoped tracks" before this change was to do:

```sql
select * from track where type = 'track'
```

This can be replaced with:

```sql
select * from track where dimension_arg_set_id is null
```

## Removal of `type` column from all non-track tables

**Date/Version**

2024-12-18/v49.0

**Symptoms**

 - An error message of the form `no such column: type`
 - The `type` column disappearing from output of queries with `SELECT *`


**Context**

NOTE: this change is very closely tied to *Change in semantic of `type` column* change, see above.

The `type` columns on tables has been around for a long time and indicated the "most specific
table" containing the track. Over time, with changes in how tables in trace processor tables are
structured (i.e. more use of the standard library, tracks with multiple dimensions), we have outgrown
the idea of "object-oriented tables" which made the `type` column meaningful.

In fact for any non-track table, the type column was almost always just equal to the name of the
table itself e.g. if you do `select type from slice`, the type column would be `slice`.

Given the very limited utility of this column, the fact that it costs us a non-trivial amount of memory
to store this information on large traces and that it pollutes the lists of columns, we are removing
this column from all non-track tables. For track tables, the purpose of this column has changed as
discussed above.

**Migrations**

It's very likely that your dependence on `type` was an accident by doing `select *` and not an active
choice. In this case, migration should be trivial by just removing references to the `type` column (e.g.
in assertions on the output of queries with `select *`).

If your workflow is now broken by this change, we would be interested in helping you resolve this issue.
Please file a bug at http://go/perfetto-bug (if you are a Googler) or
https://github.com/google/perfetto/issues/new (otherwise).
