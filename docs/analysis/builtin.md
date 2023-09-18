# PerfettoSQL Built-ins

These are functions built into C++ which reduce the amount of boilerplate which
needs to be written in SQL.

## Profile Functions

### STACK_FROM_STACK_PROFILE_FRAME

`STACK_FROM_STACK_PROFILE_FRAME(frame_id)`

#### Description

Creates a stack with just the frame referenced by `frame_id` (reference to the
[stack_profile_frame](sql-tables.autogen#stack_profile_frame) table)

#### Return Type

`BYTES`

#### Arguments

Argument | Type | Description
-------- | ---- | -----------
frame_id | StackProfileFrameTable::Id | reference to the [stack_profile_frame](sql-tables.autogen#stack_profile_frame) table

### STACK_FROM_STACK_PROFILE_CALLSITE

`STACK_FROM_STACK_PROFILE_CALLSITE(callsite_id)`

#### Description

Creates a stack by taking a `callsite_id` (reference to the
[stack_profile_callsite]](sql-tables.autogen#stack_profile_callsite) table) and
generating a list of frames (by walking the
[stack_profile_callsite]](sql-tables.autogen#stack_profile_callsite) table)

#### Return Type

`BYTES`

#### Arguments

Argument | Type | Description
-------- | ---- | -----------
callsite_id | StackProfileCallsiteTable::Id | reference to the [stack_profile_callsite]](sql-tables.autogen#stack_profile_callsite) table

### CAT_STACKS

`CAT_STACKS(([root [[,level_1 [, ...]], leaf]])`

#### Description

Creates a Stack by concatenating other Stacks. Also accepts STRING values for
which it generates a fake Frame. Null values are just ignored.

#### Return Type

`BYTES`

#### Arguments

Argument | Type | Description
-------- | ---- | -----------
root | BYTES or STRING | Stack or STRING for which a fake Frame is generated
... | BYTES or STRING | Stack or STRING for which a fake Frame is generated
leaf | BYTES or STRING | Stack or STRING for which a fake Frame is generated

### EXPERIMENTAL_PROFILE

`EXPERIMENTAL_PROFILE(stack [,sample_type, sample_units, sample_value]*)`

#### Description

Aggregation function that generates a profile in
[pprof](https://github.com/google/pprof) format from the given samples.

#### Return Type

`BYTES` ([pprof](https://github.com/google/pprof) data)

#### Arguments

Argument | Type | Description
-------- | ---- | -----------
stack | BYTES | Stack or string for which a fake Frame is generated
sample_type | STRING | Type of the sample value (e.g. size, time)
sample_units | STRING | Units of the sample value (e.g. bytes, count)
sample_value | LONG | Value for the sample

Multiple samples can be specified.

If only the `stack` argument is present, a `"samples"`, `"count"`, and `1` are
used as defaults for `sample_type`, `sample_units`, and `sample_value`
 respectively.

#### Example

CPU profile

```sql
SELECT
  perf_session_id,
  EXPERIMENTAL_PROFILE(
    STACK_FROM_STACK_PROFILE_CALLSITE(callsite_id),
    'samples',
    'count',
    1) AS profile
FROM perf_sample
GROUP BY perf_session_id
```

Heap profile

```sql
SELECT
  EXPERIMENTAL_PROFILE(
    CAT_STACKS(heap_name, STACK_FROM_STACK_PROFILE_CALLSITE(callsite_id)),
    'count',
    'count',
    count,
    'size',
    'bytes',
    size) AS profile
FROM heap_profile_allocation
WHERE size >= 0 AND count >= 0
```