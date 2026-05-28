---
name: perfetto-workflow-android-heap-dump
description:
  Use when the user has an Android trace containing a Java heap graph (heap
  dump) and wants to investigate memory usage, find leaks, or understand what is
  retaining memory. Walks through a two-phase guided workflow (automated
  first-pass triage followed by an open-ended exploratory deep dive).
---

# Investigating an Android Java Heap Dump

This skill teaches an AI agent how to investigate memory leaks and object
retention in Android Java heap dumps. It assumes the trace was recorded with the
ART perfetto data source (`java_heap_profiler`).

If the user has not yet loaded a trace into `trace_processor`, follow the
`perfetto_infra_querying_traces` skill first, then come back here.

---

## Phase 1: Mandatory First-Pass Triage (Quickstart)

This section is the mandatory first-pass triage for analyzing a heap dump. You
MUST follow these instructions verbatim before attempting any open-ended
exploratory queries. You MUST only proceed to Phase 2 (Open-Ended Exploratory
Deep Dive) if the user explicitly states that this first pass triage is not
good, not working, or inconclusive.

1.  Run the trace query using the provided compiled script to extract the heap
    dump details. The script takes the trace file as its argument and returns
    the query result as CSV.

    ```bash
    trace_processor query --query-file scripts/triage_dominator_path.sql TRACE_FILE
    ```

2.  Parse the returned string CSV to identify the columns and extract the values
    for `process_name`, `path`, `class_name`, and `self_size`. If the response
    is empty or contains only a header, inform the user that the query returned
    no matching data for this trace.

3.  Use the extracted values to formulate the following text string. You MUST
    use exactly this structure replacing the bracketed items with your extracted
    values:

    > I have a heap dump from {process_name} that with the following largest
    > dominated class path, showing class names and object counts along the
    > path: {path}
    >
    > The child at the end of that path, {class_name}, consumes {self_size}
    > bytes.
    >
    > There is likely a memory leak in this heap dump. To find where the leak is
    > likely to be, search for {process name} and find the source code for the
    > app in order to help identify where the leak may be. Use that source code
    > to assist in generating a hypothesis for the leak. If you can't find the
    > source code, tell the user that the source code can't be fond so the leak
    > can't be identified. If {class_name} is a primitive array like "byte[]"
    > and the path begins with "[ROOT_JNI_GLOBAL]", tell the user they likely
    > have a JNI reference leak. If you have a likely hypothesis for the leak,
    > reference specific locations in the code showing the leak and create an
    > implementation plan for fixing the leak.

4.  Present this generated string as a system prompt asking the underlying LLM
    to generate the final analysis response for the user. Provide the model's
    analysis cleanly.

---

## Phase 2: Open-Ended Exploratory Deep Dive

> [!IMPORTANT] \
> Do NOT execute queries in this section unless the user explicitly requests
> further exploration or confirms Phase 1 triage was insufficient.

All queries below use `$upid` and `$ts` as placeholders. Substitute the values
you picked in step 1 — `trace_processor` does not interpret `$` variables.

### Mental Model

A heap dump is a snapshot of every Java object alive in a process at a moment in
time, plus the references between them. To dive deeper than the automated
triage, you generally want to answer two questions:

1.  **What is taking up the most memory?** — sum object sizes by class or by
    _retained_ (dominated) size, not just by `self_size`. Retained size is the
    memory that would be freed if the object went away.
2.  **Why isn't it getting GC'd?** — walk the dominator tree (or the shortest
    path from a GC root) to find the chain of references keeping it alive.

### Step 1 — Confirm the trace has a heap graph and orient

```sql
INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats;

SELECT
  upid,
  graph_sample_ts,
  total_heap_size,
  reachable_heap_size,
  total_obj_count,
  reachable_obj_count,
  anon_rss_and_swap_size,
  oom_score_adj
FROM android_heap_graph_stats
ORDER BY graph_sample_ts;
```

Sanity checks at this point:

- Did any rows come back? If not, the trace doesn't contain a heap graph and you
  should stop and tell the user.
- Is `reachable_heap_size` close to `total_heap_size`? A big gap means a lot of
  memory is unreachable but not yet collected.
- How does `total_heap_size` compare to `anon_rss_and_swap_size`? If RSS is much
  larger than the Java heap, consider native/JNI allocations instead.
- Pick the `upid` and `graph_sample_ts` you'll focus on for the rest of the
  investigation.

### Step 2 — Find the classes dominating retained size

The `android_heap_graph_class_summary_tree` view aggregates the heap by the
shortest-path tree from GC roots, grouped by class name:

```sql
INCLUDE PERFETTO MODULE android.memory.heap_graph.class_summary_tree;

SELECT
  name AS class_name,
  root_type,
  self_count,
  self_size,
  cumulative_count,
  cumulative_size
FROM android_heap_graph_class_summary_tree
WHERE upid = $upid AND graph_sample_ts = $ts
ORDER BY cumulative_size DESC
LIMIT 30;
```

`cumulative_size` is the size of all objects of this class plus everything they
retain. `self_size` only counts the objects of the class itself.

### Step 3 — Use the dominator tree to find the retention chain

An object's immediate dominator is the closest ancestor that _every_ path from a
GC root must pass through.

```sql
INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

WITH suspect_objects AS (
  SELECT o.id
  FROM heap_graph_object o
  JOIN heap_graph_class c ON o.type_id = c.id
  WHERE o.upid = $upid
    AND o.graph_sample_ts = $ts
    AND c.name = 'com.example.LeakySingleton'
)
SELECT
  d.id,
  d.idom_id,
  d.dominated_obj_count,
  d.dominated_size_bytes,
  d.depth
FROM heap_graph_dominator_tree d
JOIN suspect_objects s USING (id)
ORDER BY dominated_size_bytes DESC;
```

To walk _up_ from a leaked object to its GC root, follow `idom_id` recursively.
Join each step through `heap_graph_object.type_id` → `heap_graph_class.name` to
get a human-readable retention path.

### Step 4 — Inspect specific references when needed

```sql
-- Outgoing references from a specific object.
SELECT
  r.field_name,
  r.field_type_name,
  oc.name AS owner_class,
  r.owned_id AS referent_id,
  rc.name AS referent_class,
  ro.self_size AS referent_self_size
FROM heap_graph_reference r
JOIN heap_graph_object oo ON r.owner_id = oo.id
JOIN heap_graph_class oc ON oo.type_id = oc.id
LEFT JOIN heap_graph_object ro ON r.owned_id = ro.id
LEFT JOIN heap_graph_class rc ON ro.type_id = rc.id
WHERE r.owner_id = $object_id;
```

Common things to look for here:

- An `ArrayList` / `HashMap` / `ConcurrentHashMap` whose internal array has
  thousands of slots.
- A `WeakReference` chain that turns out not to be weak in practice (a strong
  reference is also held).
- An inner class (`Outer$Inner`) with a `this$0` field pinning an Activity or
  Fragment.

---

## Phase 3: Code Search & Expert Reporting

Whether reporting on Phase 1 (Quickstart Triage) or Phase 2 (Exploratory Deep
Dive), a high-quality summary for the user MUST contain:

1.  **Orienting Context:** The process name and timestamp of the heap dump.
2.  **Primary Leak Signatures:** The top retaining class chains by
    cumulative/dominated memory size.
3.  **Code-Grounded Hypothesis & Philosophical Advice:**
    - You MUST search the workspace codebase for the application source code
      matching the process name or leaky class names to understand the
      architectural intent and lifecycle of the suspected classes.
    - Use that source code to assist in generating a grounded hypothesis for the
      leak. If you cannot find the source code, explicitly inform the user that
      the source code cannot be found, so the exact leak mechanism cannot be
      definitively verified.
    - If `class_name` is a primitive array like `byte[]` and the path begins
      with `[ROOT_JNI_GLOBAL]`, advise the user they likely have a JNI global
      reference leak.
    - Provide expert philosophical advice on the architectural approach to take
      (e.g., decoupling static singletons, adopting weak references, or clearing
      listeners in `onDestroy`).
4.  **Actionable Implementation Plan:** Reference specific locations in the
    codebase (filenames and line numbers) showing the leak and create a concrete
    implementation plan for fixing it.

Always keep the underlying SQL and object IDs available so the user can audit.
Do not make claims about what the code is doing without inspecting the
references and matching source code.

## Reference

- Java heap profiler:
  <https://perfetto.dev/docs/data-sources/java-heap-profiler>
- PerfettoSQL language tour:
  <https://perfetto.dev/docs/analysis/perfetto-sql-getting-started>
- Generated stdlib reference: <https://perfetto.dev/docs/analysis/stdlib-docs>
