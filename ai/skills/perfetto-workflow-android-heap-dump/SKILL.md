---
name: perfetto-workflow-android-heap-dump
description: Use when the user has an Android trace containing a Java heap
  graph (heap dump) and wants to investigate memory usage, find leaks, or
  understand what is retaining memory. Walks through a guided workflow
  built on the heap_graph_* tables and the android.memory.heap_graph
  stdlib.
---

# Investigating an Android Java heap dump

This skill is the recommended first pass when the user asks "what's wrong
with this heap dump?", "why is this process using so much memory?", or
"what's leaking?". It assumes the trace was recorded with the ART perfetto
data source and contains at least one Java heap graph.

If the user has not yet loaded a trace into `trace_processor`, follow
the `perfetto-infra-querying-traces` skill first, then come back here.
Recording-side reference for heap dumps:
<https://perfetto.dev/docs/data-sources/java-heap-profiler>.

> All queries below use `$upid` and `$ts` as placeholders. Substitute the
> values you picked in step 1 — `trace_processor` does not interpret `$`
> variables.

## Mental model

A heap dump is a snapshot of every Java object alive in a process at a
moment in time, plus the references between them. To find a leak you
generally want to answer two questions:

1. **What is taking up the most memory?** — sum object sizes by class or
   by *retained* (dominated) size, not just by `self_size`. Retained size
   is the memory that would be freed if the object went away.
2. **Why isn't it getting GC'd?** — walk the dominator tree (or the
   shortest path from a GC root) to find the chain of references keeping
   it alive.

The Perfetto stdlib has prebuilt views for both. Reach for those before
joining `heap_graph_object` and `heap_graph_reference` by hand. Browse the
modules under `android.memory.heap_graph.*` in the stdlib reference:
<https://perfetto.dev/docs/analysis/stdlib-docs>.

## Step 1 — Confirm the trace has a heap graph and orient

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

- Did any rows come back? If not, the trace doesn't contain a heap graph
  and you should stop and tell the user.
- Is `reachable_heap_size` close to `total_heap_size`? A big gap means a
  lot of memory is unreachable but not yet collected — interesting on its
  own but not the typical "leak" pattern.
- How does `total_heap_size` compare to `anon_rss_and_swap_size`? If RSS
  is much larger than the Java heap, the bloat is probably not Java —
  consider native allocations instead.
- Pick the `upid` and `graph_sample_ts` you'll focus on for the rest of
  the investigation. If there are multiple dumps, the earliest one
  usually shows the steady state; later dumps show growth.

Join with `process` to turn `upid` into a process name when reporting back
to the user — never expose raw `upid` values.

## Step 2 — Find the classes dominating retained size

The `android_heap_graph_class_summary_tree` view aggregates the heap by
the shortest-path tree from GC roots, grouped by class name. It's the
fastest way to spot "this one class is holding most of the heap":

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

`cumulative_size` is the size of all objects of this class plus everything
they retain — this is what to sort by to find the dominant retainer.
`self_size` only counts the objects of the class itself.

Patterns that are usually worth flagging to the user:

- A single class with a `cumulative_size` that's a large fraction of
  `reachable_heap_size`.
- An unexpectedly large `self_count` for a class that "should" only have
  a handful of instances (Activities, Fragments, application singletons).
- A `root_type` of `ROOT_JAVA_FRAME` or `ROOT_JNI_GLOBAL` retaining a lot
  — these often indicate a native or JNI leak.

## Step 3 — Use the dominator tree to find the retention chain

Once a suspicious class is identified, the dominator tree tells you what
is *uniquely* keeping its instances alive. An object's immediate
dominator is the closest ancestor that *every* path from a GC root must
pass through.

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

To walk *up* from a leaked object to its GC root, follow `idom_id`
recursively (it's NULL once you hit the super-root). Join each step
through `heap_graph_object.type_id` → `heap_graph_class.name` to get a
human-readable retention path.

For a per-class rollup of the dominator tree (often easier to read than
per-object), use `android.memory.heap_graph.dominator_class_tree`
instead.

## Step 4 — Inspect specific references when needed

The dominator tree shows *one* retaining edge per object (the dominator).
If you need the full set of incoming/outgoing references — e.g. to
explain why an object can't be collected even though the dominator looks
benign — query `heap_graph_reference` directly. Note that
`heap_graph_reference.owner_id` is the source object id; you do not need
to join through `reference_set_id`.

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

- An `ArrayList` / `HashMap` / `ConcurrentHashMap` whose internal array
  has thousands of slots — the leaked container is the suspect.
- A `WeakReference` or `SoftReference` chain that turns out not to be
  weak in practice (a strong reference is also held).
- An inner class (`Outer$Inner`) with a `this$0` field pinning an
  Activity or Fragment — classic Android leak.

## What to report back

A good summary for the user contains:

1. The process name and the timestamp of the heap dump.
2. Total reachable heap size and the top retainers by `cumulative_size`,
   with class names, not raw IDs.
3. For the dominant retainer, the chain from a GC root to one example
   object, expressed as `Class -> field -> Class -> field -> …`.
4. A hypothesis stated as a hypothesis, not a fact ("this looks like a
   leaked Activity retained via the inner Listener; the next step is to
   confirm by …").

Always keep the underlying SQL and object IDs available so the user can
audit. Do not make claims about what the code is doing without inspecting
the references.

## Reference

- Java heap profiler (recording side):
  <https://perfetto.dev/docs/data-sources/java-heap-profiler>
- PerfettoSQL language tour (with a heap-graph example):
  <https://perfetto.dev/docs/analysis/perfetto-sql-getting-started>
- Generated stdlib reference (browse `android.memory.heap_graph.*`):
  <https://perfetto.dev/docs/analysis/stdlib-docs>
