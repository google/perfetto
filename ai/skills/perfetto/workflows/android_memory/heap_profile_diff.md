# Diffing Android Java & Native Heap Allocation Profiles

Compares two Android heap allocation profiles (before vs after) captured with
`heapprofd`, either Java (`com.android.art`) or native (`libc.malloc`).

- **Java allocation profiles (`com.android.art`) record allocations only, never
  GC frees.** In `com.android.art`, `unreleased_bytes` equals `alloc_bytes` by
  construction, so use these profiles strictly for **allocation churn** (GC
  pressure from temporary objects). To find retained Java leaks, use
  [heap_dump_diff.md]($SKILL_ROOT/workflows/android_memory/heap_dump_diff.md).
- **Native allocation profiles (`libc.malloc`) record both allocations and
  frees.** Use `unreleased_bytes` (`SUM(size)`) for native leaks / active heap
  growth, and `alloc_bytes` (`SUM(MAX(size, 0))`) for allocation churn.

## Step 1: Load both traces into warm sessions and check available profiles

Load each trace once into a named session and list the recorded `(upid,
process_name, heap_name)` profiles:

```bash
trace_processor server kill before_tp 2>/dev/null; trace_processor server kill after_tp 2>/dev/null
trace_processor server unix --name before_tp --daemonize <before_trace>
trace_processor server unix --name after_tp --daemonize <after_trace>
trace_processor query --remote before_tp -f - << 'EOF'
    SELECT
      a.upid,
      COALESCE(p.name, 'pid=' || p.pid) AS process_name,
      a.heap_name,
      SUM(MAX(a.size, 0)) AS alloc_bytes,
      SUM(a.size) AS unreleased_bytes
    FROM heap_profile_allocation a
    JOIN process p USING (upid)
    GROUP BY a.upid, process_name, a.heap_name;
EOF
```

Kill both sessions when finished:
`trace_processor server kill before_tp && trace_processor server kill after_tp`.

## Step 2: Diff profile totals, functions, and callstacks

Diff the two sessions:

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/diff_heap_profiles.py before_tp after_tp
```

The script queries `heap_profile_allocation` and
`_android_heap_profile_callstacks_for_allocations!` in both sessions, collapses
single-child pass-through wrapper frames (e.g. `main -> ZygoteInit -> Looper`)
whose cumulative totals are identical to their child's, and reports the profile
summary, **Top Functions** by cumulative growth (`--top-functions`, default 10),
and **Top Callstacks by Growth** (`--top-callstacks`, default 10).

If Step 1 showed multiple processes or heaps in a session, select the target
profile with `--before-upid` / `--after-upid` and `--heap-name` (e.g.
`com.android.art` or `libc.malloc`).

## Step 3: Inspect callstacks and attribute in code

Leaf frames in `com.android.art` (e.g. `art::gc::Heap::AllocWithNewTLAB`) and
`libc.malloc` (e.g. `malloc`, `operator new`) are runtime allocators shared by
every call site. Attribute the regression to the **application or library caller
frame** immediately above the runtime frames in the callstack (e.g.
`ImageDecoder.decodeBitmap` or `libfoo.so`).

If you need to inspect raw frames or source line numbers for a specific
function in `after_tp`, query `android_heap_profile_summary_tree` using
`-f - << 'EOF'` so `$` in inner class names (e.g. `Foo$Bar`) is not expanded by
the shell:

```bash
trace_processor query --remote after_tp -f - << 'EOF'
    INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

    SELECT
      id,
      parent_id,
      name AS function_name,
      mapping_name,
      source_file,
      line_number,
      self_size AS self_unreleased_bytes,
      cumulative_size AS cumulative_unreleased_bytes,
      self_alloc_size AS self_alloc_bytes,
      cumulative_alloc_size AS cumulative_alloc_bytes
    FROM android_heap_profile_summary_tree
    WHERE name GLOB '*<FUNCTION_SUBSTRING>*'
    ORDER BY cumulative_alloc_bytes DESC;
EOF
```

## Further investigation and reporting

- **Java allocation churn (`com.android.art`):** Once you have identified the
  regressed Java methods and callstacks, follow Phase 2 Step 3 (walking a
  specific callstack ID) and Phase 3 (code search & optimization plan) in
  [java_allocation_profile.md]($SKILL_ROOT/workflows/android_memory/java_allocation_profile.md).
- **Native heap growth & churn (`libc.malloc`):** Once you have identified the
  regressed native functions and shared libraries, follow Phase 2 Step 3 and
  Phase 3 in [native_heap.md]($SKILL_ROOT/workflows/android_memory/native_heap.md).

## Reporting and query rules

1.  Report before and after totals with the delta (e.g.
    `120,000 -> 4,800,000 B (+4,680,000 B)` and `30 -> 1,200 (+1,170)`
    allocations), the responsible function/method, its owning shared library or
    binary (`mapping_name`, e.g. `libfoo.so`), and the callstack path.
2.  For `com.android.art`, report **allocated bytes and allocation count**
    (churn) only — never claim a Java memory leak from `unreleased_bytes` in an
    allocation profile.
3.  `heapprofd` uses Poisson sampling (`sampling_interval_bytes`, default 4096 B).
    Ignore callstacks whose delta is near a single sampling interval, and check
    `SELECT name, value FROM stats WHERE name LIKE 'heapprofd%' AND value > 0`
    if a profile appears truncated.
