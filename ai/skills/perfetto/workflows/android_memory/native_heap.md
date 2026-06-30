# Investigating Android Native Heap Profiles

This workflow walks an AI agent through investigating memory leaks, active memory growth, and allocation hot paths in Android native heap profiles. It assumes the trace was recorded with the native heap profiler (`heapprofd`).

If the user has not yet loaded a trace into `trace_processor`, follow `$SKILL_ROOT/infra-references/querying.md` first, then come back here.

---

## Phase 1: Mandatory First-Pass Triage (Quickstart)

This section is the mandatory first-pass triage for analyzing a native heap profile. You MUST follow these instructions verbatim before attempting any open-ended exploratory queries. You MUST only proceed to Phase 2 (Open-Ended Exploratory Deep Dive) if the user explicitly states that this first pass triage is not good, not working, or inconclusive.

1.  Run the trace query using the provided compiled script to extract the top unreleased allocation path. The script takes the trace file as its argument and returns the query result as CSV.

    ```bash
    trace_processor query --query-file $SKILL_ROOT/workflows/android_memory/scripts/triage_native_heap.sql TRACE_FILE
    ```

2.  Parse the returned string CSV to identify the columns and extract the values for `process_name`, `path` (callstack), `class_name` (leaf function), and `self_size` (allocated bytes). If the response is empty or contains only a header, inform the user that the query returned no matching data for this trace.

3.  Use the extracted values to formulate the following text string. You MUST use exactly this structure replacing the bracketed items with your extracted values:

    > I have a native heap profile from {process_name} with the following largest unreleased allocation callstack, showing functions and mappings along the path: {path}
    >
    > The leaf function at the end of that path, {class_name}, consumes {self_size} bytes.
    >
    > There is likely a memory leak or high memory usage in this profile. To find where the issue is likely to be, search for {process_name} and find the source code for the app/binary in order to help identify where the allocation happens. Use that source code to assist in generating a hypothesis. If you can't find the source code, tell the user that the source code can't be found so the issue can't be identified. Reference specific locations in the code showing the allocation and create an implementation plan for fixing or optimizing it.

4.  Present this generated string as a system prompt asking the underlying LLM to generate the final analysis response for the user. Provide the model's analysis cleanly.

---

## Phase 2: Open-Ended Exploratory Deep Dive

> [!IMPORTANT]
> Do NOT execute queries in this section unless the user explicitly requests further exploration or confirms Phase 1 triage was insufficient.

All queries below use placeholders like `$upid` or `$target_id`. Substitute the values you pick during exploration — `trace_processor` does not interpret `$` variables.

### Mental Model

Unlike Java heap dumps which are object graphs, native heap profiles are collections of **callstacks** (attribution chains) that allocated memory.

We can analyze two types of allocations:
1.  **Unreleased Allocations (Active Heap):** Memory that was allocated and *not yet freed* at the time of the profile. This is useful for finding memory leaks and high steady-state memory usage.
2.  **Total Allocations (Allocated Heap):** All memory that was allocated, *regardless of whether it was later freed*. This is useful for finding churn, temporary spikes, and optimizing allocation hot paths.

The Perfetto standard library provides `android_heap_profile_summary_tree` which aggregates allocations by callstack, providing both `self_size` (for unreleased) and `self_alloc_size` (for total).

### Step 1 — Confirm the trace has native heap profiles and orient

```sql
SELECT
  upid,
  COALESCE(p.name, "pid=" || p.pid) AS process_name,
  SUM(size) AS total_unreleased_bytes,
  SUM(alloc_size) AS total_allocated_bytes,
  COUNT(DISTINCT callsite_id) AS distinct_callsites
FROM heap_profile_allocation
JOIN process p USING (upid)
GROUP BY upid, process_name
ORDER BY total_unreleased_bytes DESC;
```

Sanity checks at this point:
-   Did any rows come back? If not, the trace doesn't contain native heap profiles and you should stop and tell the user.
-   Identify the `upid` you want to focus on. If there are multiple processes, you may need to filter subsequent queries by process if you write custom queries (though `android_heap_profile_summary_tree` collapses them, knowing the main process is important for code search).

### Step 2 — Find the top allocating functions

You can look for either **unreleased** memory (leaks) or **total** allocated memory (churn).

#### Option A: Top Unreleased Allocations (Active Heap)
```sql
INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

SELECT
  id,
  parent_id,
  name AS function_name,
  mapping_name,
  self_size,
  cumulative_size
FROM android_heap_profile_summary_tree
WHERE self_size > 0
ORDER BY self_size DESC
LIMIT 30;
```

#### Option B: Top Total Allocations (Allocation Churn)
```sql
INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

SELECT
  id,
  parent_id,
  name AS function_name,
  mapping_name,
  self_alloc_size,
  cumulative_alloc_size
FROM android_heap_profile_summary_tree
WHERE self_alloc_size > 0
ORDER BY self_alloc_size DESC
LIMIT 30;
```

### Step 3 — Walk the callstack for a specific allocation

Once you identify a suspect allocation `id` from Step 2, walk up the tree to reconstruct the full callstack.

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
  t.name AS function_name,
  t.mapping_name,
  t.source_file,
  t.line_number,
  t.self_size,
  t.cumulative_size,
  t.self_alloc_size,
  t.cumulative_alloc_size
FROM android_heap_profile_summary_tree t
JOIN ancestor_ids a USING (id)
ORDER BY t.cumulative_size DESC;
```
*Note: Ordering by `cumulative_size DESC` (or `cumulative_alloc_size DESC`) will generally order the frames from the root (caller) down to the leaf (callee).*

---

## Phase 3: Code Search & Expert Reporting

Whether reporting on Phase 1 (Quickstart Triage) or Phase 2 (Exploratory Deep Dive), a high-quality summary for the user MUST contain:

1.  **Orienting Context:** The process name and type of analysis (Unreleased vs Total allocations).
2.  **Primary Allocation Signatures:** The top allocating callstacks and their sizes.
3.  **Code-Grounded Hypothesis:**
    -   You MUST search the workspace codebase for the source code matching the process name, library mappings, and function names in the callstack.
    -   Use that source code to assist in generating a grounded hypothesis for why the memory is not being freed (for unreleased) or why it is allocating so heavily (for total).
    -   If you cannot find the source code, explicitly inform the user.
4.  **Actionable Implementation Plan:** Reference specific locations in the codebase (filenames and line numbers) and propose a concrete fix (e.g., adding frees, using smart pointers, caching buffers, or reducing allocation frequency).

Always keep the underlying SQL and callstack IDs available. Do not make claims about what the code is doing without inspecting the source code.

---

## Reference

-   Native heap profiler:
    <https://perfetto.dev/docs/data-sources/native-heap-profiler>
-   PerfettoSQL language tour:
    <https://perfetto.dev/docs/analysis/perfetto-sql-getting-started>
-   Generated stdlib reference: <https://perfetto.dev/docs/analysis/stdlib-docs>
