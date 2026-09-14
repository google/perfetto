# Diffing Android Java & Native Heap Allocation Profiles

This workflow guides an AI agent through comparing two Android Perfetto heap
allocation profiles (`before_trace` vs `after_trace`) captured with `heapprofd`
for Java (`com.android.art`) or Native (`libc.malloc`) heaps. Use this workflow
whenever investigating memory allocation churn, GC pressure, temporary object
spikes, or active native heap growth across code changes, app versions, test runs,
or before/after user journeys.

## Mental Model: Java vs Native Profiles & How `heapprofd` Sampling Works

### 1. Java (`com.android.art`) vs Native (`libc.malloc`) Allocation Profiles

-   **Java Heap Allocation Profiles (`com.android.art`) record ONLY allocations
    (no releases / GC frees)**:
    -   ART allocation profiling (`heaps: "com.android.art"`, Android 12+)
        captures callstacks only when Java objects are created—it **does not**
        track when objects are garbage-collected or freed.
    -   Consequently, in `com.android.art` profiles, every allocation has no
        matching free event (`self_size == self_alloc_size` and
        `cumulative_size == cumulative_alloc_size`).
    -   Use `com.android.art` profiles strictly to measure **Java allocation
        churn** (total bytes and objects allocated over the trace window causing
        GC pressure). To determine whether Java objects remain live in memory
        (retained/leaked), use Java heap dumps (`heap_dump_diff.md`).
-   **Native Heap Allocation Profiles (`libc.malloc`) record BOTH allocations and
    frees**:
    -   Native profiling hooks `malloc`/`free` and C++ `operator new`/`delete`,
        providing two distinct metrics:
        -   **Total Allocation Churn (`self_alloc_size` /
            `cumulative_alloc_size`)**: All bytes allocated during the trace
            window, *including allocations that were later freed*.
        -   **Unreleased Allocations (`self_size` / `cumulative_size`)**: Bytes
            allocated during the trace window that were *not freed* by the end of
            the trace (active native memory growth / leaks).

### 2. How `heapprofd` Sampling Works (`sampling_interval_bytes`)

-   **Poisson / Exponential Random Sampling**: To minimize runtime overhead,
    `heapprofd` does not record every allocation. Instead, it samples allocations
    probabilistically using an exponential distribution parameterized by
    `sampling_interval_bytes` (default `4096` bytes). Conceptually, each
    allocated byte has a $1 / \text{sampling\_interval\_bytes}$ probability of
    triggering a sample, avoiding bias against periodic allocation patterns.
-   **Scaled Allocation Sizes**: When an allocation is sampled, `heapprofd`
    scales its reported byte size (`heap_profile_allocation.size`) by
    multiplying the number of exponential draws triggered within that allocation
    by `sampling_interval_bytes`. Allocations large enough to have a $>99\%$
    probability of being sampled bypass scaling and record their exact size.
-   **Memoryless Property & Diffing Guardrails**:
    -   Because exponential sampling is memoryless, a callstack that allocates
        many small objects over time (e.g., 1,000 allocations of 64 bytes = 64
        KB total) has the same sampling probability and low expected error as a
        single 64 KB allocation.
    -   However, callstacks with small total allocations (near a single
        `sampling_interval_bytes`) exhibit random run-to-run sampling noise.
    -   When comparing `before_trace` and `after_trace`, ensure both traces use
        the same `sampling_interval_bytes` and focus on callstacks whose delta
        is **significantly larger than `sampling_interval_bytes`** (e.g., $\ge
        10\times$ the sampling interval).

## Phase 1: Automated End-to-End Allocation Profile Diff (`diff_heap_profiles.py`)

### Step 1.1: Discover Available Processes & Heaps

Before running the diff script, verify the exact `process_name` (e.g.,
`com.android.systemui.qs.panel`) and `heap_name` (`com.android.art` or
`libc.malloc`) recorded in the trace:

```bash
$SKILL_ROOT/bin/trace_processor -Q "SELECT p.name AS process_name, a.heap_name, COUNT(*) AS samples FROM heap_profile_allocation a JOIN process p USING (upid) GROUP BY 1, 2;" <path/to/before.perfetto-trace>
```

### Step 1.2: Run `diff_heap_profiles.py`

Run the bundled Python diffing script directly against your baseline
(`before_trace`) and candidate (`after_trace`) `.perfetto-trace` files using the
discovered `--process_name` and `--heap_name`. The script automatically invokes
`trace_processor` using `query_heap_profile_callstacks.sql` and
`query_heap_profile_summary_tree.sql` and emits a concise, structured Markdown
comparison report directly to stdout (print directly to stdout rather than
redirecting to a file and reading it back):

#### For Java Allocation Churn (`com.android.art`)

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/diff_heap_profiles.py \
  --process_name <process_name> \
  --heap_name com.android.art \
  --before_trace <path/to/before.perfetto-trace> \
  --after_trace <path/to/after.perfetto-trace> \
  [--function_filter <substring>] \
  [--before_index <idx> --after_index <idx>] \
  [--before_ts <dump_ts> --after_ts <dump_ts>] \
  [--output_file <report.md>] \
  --sort_by alloc_size \
  --top_n 10
```

#### For Native Allocation Growth & Churn (`libc.malloc`)

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/diff_heap_profiles.py \
  --process_name <process_name> \
  --heap_name libc.malloc \
  --before_trace <path/to/before.perfetto-trace> \
  --after_trace <path/to/after.perfetto-trace> \
  [--function_filter <substring>] \
  [--before_index <idx> --after_index <idx>] \
  [--before_ts <dump_ts> --after_ts <dump_ts>] \
  [--output_file <report.md>] \
  --sort_by unreleased_size \
  --top_n 10
```

-   **Choosing the Ranking Metric (`--sort_by`, default `alloc_size`)**:
    -   `alloc_size` / `alloc_count`: total bytes / allocations made during the
        trace window, freed or not. This is the churn and GC-pressure view, and
        the **only** meaningful choice for `com.android.art`, which records no
        frees.
    -   `unreleased_size` / `unreleased_count`: bytes / allocations still live at
        the end of the window. Meaningful for `libc.malloc` only; this is the
        native leak and steady-state RSS view.
-   **Narrowing to a Suspect Frame (`--function_filter <substring>`)**: Restricts
    both the callstack and the function tables to stacks containing a matching
    frame. Use it once a metric or an earlier run has already named a suspect
    method, class or `.so`.

-   **Disambiguating Multiple Profile Dumps / Continuous Snapshots in a Trace**:
    -   When `continuous_dump_config` or periodic `killall -USR1 heapprofd`
        snapshots are enabled, a single trace contains multiple delta-encoded
        dump slices (`heap_profile_allocation.ts`).
    -   By default, when comparing two different trace files, the script sums
        allocations across the entire trace window and lists all available dump
        timestamps (`ts`) in the report header.
    -   When comparing two snapshots **within the same trace file**
        (`--before_trace trace.pftrace --after_trace trace.pftrace`), the script
        automatically compares the **first** dump slice (`--before_index 0`)
        against the **last** dump slice (`--after_index -1`). You can also
        explicitly select any continuous dump interval by index
        (`--before_index` / `--after_index`) or exact timestamp (`--before_ts` /
        `--after_ts`).

### How to Interpret the Report

The report generated by `diff_heap_profiles.py` contains four key sections:

1.  **Process & Heap Overview**: Shows total allocated churn (`self_alloc_size`),
    total unreleased heap (`self_size`), and total allocation count
    (`self_alloc_count`) deltas between `before` and `after`.
2.  **Top Regressed Callstacks (Sorted by `--sort_by`, default `alloc_size`)**:
    Identifies the exact root-to-leaf callstack paths (`Root -> ... -> Leaf`)
    with the largest increase in the selected `--sort_by` metric (`alloc_size`,
    `alloc_count`, `unreleased_size`, or `unreleased_count`).
3.  **Top Regressed Callstacks Ranked by Unreleased Size Delta (`self_size`)**:
    Identifies the exact callstack paths responsible for un-freed memory growth.
4.  **Top Regressed Functions / Methods (Cumulative Allocation Churn Delta)**:
    Ranks individual Java methods and C/C++ functions anywhere along the
    callstack by their cumulative allocation churn growth (`cumulative_alloc_size`
    delta) and cumulative unreleased growth (`cumulative_size` delta) from
    `android_heap_profile_summary_tree`.

## Phase 2: Underlying PerfettoSQL Queries

If you need to inspect or customize the SQL queries run by
`diff_heap_profiles.py`:

### Step 2.1: Per-Callstack Extraction (`query_heap_profile_callstacks.sql`)

```bash
trace_processor -q $SKILL_ROOT/workflows/android_memory/scripts/query_heap_profile_callstacks.sql <trace_file>
```

Uses `android.memory.heap_profile.summary_tree`, `heap_profile_allocation`,
`_callstack_spc_forest`, and `graphs.scan` to reconstruct full callstack paths
from root to leaf and aggregate `self_size`, `self_alloc_size`, `self_count`,
`self_alloc_count`, `cumulative_size`, and `cumulative_alloc_size`.

### Step 2.2: Per-Function Summary Tree (`query_heap_profile_summary_tree.sql`)

```bash
trace_processor -q $SKILL_ROOT/workflows/android_memory/scripts/query_heap_profile_summary_tree.sql <trace_file>
```

Aggregates `android_heap_profile_summary_tree` by `(function_name, mapping_name)`
so caller frames (such as `ImageDecoder.decodeBitmap` or
`NativeRenderer.allocateFrameBuffer`) are ranked by their `cumulative_alloc_size`
and `cumulative_size` deltas even when leaf allocator frames (`AllocWithNewTLAB`
or `malloc`) are shared across many callers.

## Phase 3: Code Search & Root-Cause Attribution

Once `diff_heap_profiles.py` surfaces the top regressed callstack paths and
methods:

1.  **Locate the Caller Frame**: Leaf frames in `com.android.art`
    (`art::gc::Heap::AllocWithNewTLAB`) and `libc.malloc` (`malloc` /
    `operator new`) are runtime allocators. Look at the highest application or
    framework method in the callstack path or top function table (e.g.,
    `ImageDecoder.decodeBitmap` or `NativeRenderer.allocateFrameBuffer`).
2.  **Search the Codebase**: Use code search to inspect the regressed Java/Kotlin
    method or C/C++ function:
    -   For **Java Allocation Churn**: look for temporary object allocations
        inside loops, `StringBuilder` / string concatenation in hot paths,
        autoboxing into boxed collections, or missing object pools.
    -   For **Native Allocation Growth/Churn**: look for missing `free()` /
        `delete`, growing `std::vector` / `std::deque` buffers, or per-frame
        native allocations.

## Reference: Troubleshooting

-   **The report is empty, or the process/heap is missing**: The trace contains
    no `heap_profile_allocation` rows for that `(process, heap)` pair. Confirm
    what was actually recorded before concluding anything:

    ```sql
    SELECT p.name AS process_name, a.heap_name,
           COUNT(*) AS samples, MIN(a.ts) AS first_ts, MAX(a.ts) AS last_ts
    FROM heap_profile_allocation a
    JOIN process p USING (upid)
    GROUP BY process_name, a.heap_name;
    ```

    An empty result means `heapprofd` never attached — usually a non-profileable
    build, a process name that never matched, or a process that started after
    profiling began.
-   **Native frames show as hex addresses or `[unknown]`**: The `.so` symbols
    were not available at trace time. The byte deltas are still valid; attribute
    them by mapping name (`libfoo.so`) and symbolize before naming a function.
-   **Deltas look implausibly large or noisy**: Check that both traces used the
    same `sampling_interval_bytes`, and ignore callstacks whose delta is within
    roughly `10x` the sampling interval. Also check for `heapprofd` data loss:

    ```sql
    SELECT name, value FROM stats
    WHERE name LIKE 'heapprofd%' AND value > 0;
    ```

    Non-zero buffer-corruption or sample-rejection counters mean the profile is
    incomplete and deltas are understated.
-   **`com.android.art` shows a large `unreleased_size`**: Ignore it. ART
    allocation profiles record no frees, so `self_size` is identical to
    `self_alloc_size` by construction and says nothing about retention. Use a
    Java heap dump diff (`heap_dump_diff.md`) to reason about retained memory.
-   **No callstack regressed**: That is a valid result. Report the process-level
    churn and unreleased deltas and say the regression is not visible in this
    heap, rather than promoting the largest noise row to a root cause.
