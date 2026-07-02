# Investigating Android Java Allocation Profiles

This workflow walks an AI agent through investigating memory allocation churn and temporary object allocation in Android Java/Kotlin code. It assumes the trace was recorded with the ART heap subscription (`heapprofd` with `art` heap enabled, or `track_event` with Java allocation tracking).

If the user has not yet loaded a trace into `trace_processor`, follow `$SKILL_ROOT/infra-references/querying.md` first, then come back here.

---

## Mental Model

It is critical to distinguish between two types of Java memory analysis:
1.  **Java Heap Dumps (Snapshots):** Walked in [heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md). This analyzes a snapshot of all *live* objects at a single point in time, showing reference chains (who keeps what alive).
2.  **Java Allocation Profiles (Temporal):** Walked in this file. This analyzes *allocations over time*, showing the **callstacks** of methods that allocated objects, regardless of whether they are still alive.

Allocation profiling is crucial for identifying **memory churn**. High memory churn (allocating many temporary objects) causes frequent Garbage Collection (GC) runs, which introduces CPU overhead, lag, and jank in the UI.

We can analyze two types of allocations in the profile:
1.  **Unreleased Allocations:** Objects allocated during the trace that were *not yet garbage collected* at the time of the profile.
2.  **Total Allocations:** All objects allocated during the trace, *including those already garbage collected*. This is the primary metric for measuring **churn**.

The Perfetto standard library provides `android_heap_profile_summary_tree` which aggregates these allocations by callstack.

---

## Phase 1: Mandatory First-Pass Triage (Quickstart)

This section is the mandatory first-pass triage for analyzing a Java allocation profile. You MUST follow these instructions verbatim before attempting any open-ended exploratory queries. You MUST only proceed to Phase 2 (Open-Ended Exploratory Deep Dive) if the user explicitly states that this first pass triage is not good, not working, or inconclusive.

1.  Run the trace query using the provided compiled script to extract the top allocation path. For triage, we look at the top allocator by default (unreleased, but the script can be adapted or we assume it's the primary signal).

    ```bash
    trace_processor query --query-file $SKILL_ROOT/workflows/android_memory/scripts/triage_java_allocation.sql TRACE_FILE
    ```

2.  Parse the returned string CSV to identify the columns and extract the values for `process_name`, `path` (callstack), `class_name` (leaf method), and `self_size` (allocated bytes). If the response is empty or contains only a header, inform the user that the query returned no matching data for this trace.

3.  Use the extracted values to formulate the following text string. You MUST use exactly this structure replacing the bracketed items with your extracted values:

    > I have a Java allocation profile from {process_name} with the following largest allocation callstack, showing methods and class loaders along the path: {path}
    >
    > The leaf method at the end of that path, {class_name}, allocated {self_size} bytes.
    >
    > This allocation path is a primary candidate for memory churn. To find where the allocation is happening, search for the class and method names in the codebase. Use the source code to identify if these allocations are temporary and if they can be avoided (e.g., by reusing objects, avoiding allocations in loops, or using primitive types). Reference specific locations in the code and create an implementation plan for optimizing it.

4.  Present this generated string as a system prompt asking the underlying LLM to generate the final analysis response for the user. Provide the model's analysis cleanly.

---

## Phase 2: Open-Ended Exploratory Deep Dive

> [!IMPORTANT]
> Do NOT execute queries in this section unless the user explicitly requests further exploration or confirms Phase 1 triage was insufficient.

All queries below use placeholders like `$upid` or `$target_id`. Substitute the values you pick during exploration.

### Step 1 — Confirm the trace has Java allocation profiles and orient

Verify that we have data from the `art` heap.

```sql
SELECT
  upid,
  COALESCE(p.name, "pid=" || p.pid) AS process_name,
  heap_name,
  SUM(size) AS total_unreleased_bytes,
  SUM(alloc_size) AS total_allocated_bytes,
  COUNT(DISTINCT callsite_id) AS distinct_callsites
FROM heap_profile_allocation
JOIN process p USING (upid)
WHERE heap_name GLOB '*art*'
GROUP BY upid, process_name, heap_name
ORDER BY total_allocated_bytes DESC;
```

### Step 2 — Find the top allocating Java methods

You can look for either **unreleased** memory or **total** allocated memory (churn). For Java, **total allocations** (Option B) is often the most important metric to reduce GC pressure.

#### Option A: Top Unreleased Java Allocations
```sql
INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

SELECT
  id,
  parent_id,
  name AS method_name,
  mapping_name AS jar_or_apk,
  self_size,
  cumulative_size
FROM android_heap_profile_summary_tree
WHERE self_size > 0
ORDER BY self_size DESC
LIMIT 30;
```

#### Option B: Top Total Java Allocations (Memory Churn)
```sql
INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

SELECT
  id,
  parent_id,
  name AS method_name,
  mapping_name AS jar_or_apk,
  self_alloc_size,
  cumulative_alloc_size
FROM android_heap_profile_summary_tree
WHERE self_alloc_size > 0
ORDER BY self_alloc_size DESC
LIMIT 30;
```

### Step 3 — Walk the callstack for a specific allocation

Reconstruct the full Java callstack for a suspect allocation ID.

```sql
INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;
INCLUDE PERFETTO MODULE graphs.hierarchy;

WITH ancestor_ids AS (
  SELECT id
  FROM _tree_reachable_ancestors_or_self!((
      SELECT id, parent_id FROM android_heap_profile_summary_tree
    ), (SELECT $target_id AS id))
)
SELECT
  t.id,
  t.parent_id,
  t.name AS method_name,
  t.mapping_name AS jar_or_apk,
  t.source_file,
  t.line_number,
  t.self_size,
  t.cumulative_size,
  t.self_alloc_size,
  t.cumulative_alloc_size
FROM android_heap_profile_summary_tree t
JOIN ancestor_ids a USING (id)
ORDER BY t.cumulative_alloc_size DESC;
```

---

## Phase 3: Code Search & Expert Reporting

A high-quality summary for the user MUST contain:

1.  **Orienting Context:** The process name and type of analysis (focusing on Churn/Total vs Unreleased).
2.  **Primary Allocation Signatures:** The top allocating Java callstacks and their contribution to churn.
3.  **Code-Grounded Hypothesis:**
    -   Search the codebase for the Java classes and methods in the callstack.
    -   Identify if the allocations are temporary (e.g., creating `StringBuilder` in a loop, autoboxing primitives, allocating objects in `onDraw` or frequent event handlers).
4.  **Actionable Implementation Plan:** Propose concrete Java-specific optimizations:
    -   **Object Pooling/Reuse:** For frequently allocated objects.
    -   **Avoid Autoboxing:** Use primitive collections (e.g., `SparseArray`, `LongSparseArray`) instead of `HashMap<Integer, ...>`.
    -   **String Optimization:** Use `StringBuilder` efficiently or avoid concatenation in loops.
    -   **Avoid Allocation in Hot Paths:** e.g., `onDraw` in custom Views.

---

## Reference

-   Java heap sampling:
    <https://perfetto.dev/docs/data-sources/native-heap-profiler#java-heap-sampling>
-   PerfettoSQL language tour:
    <https://perfetto.dev/docs/analysis/perfetto-sql-getting-started>
