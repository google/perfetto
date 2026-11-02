# Investigating an Android Java Heap Dump

This workflow walks an AI agent through investigating memory leaks and object
retention in Android Java heap dumps. It assumes the trace was recorded with the
ART perfetto data source (`java_heap_profiler`).

If the user has not yet loaded a trace into `trace_processor`, follow
`$SKILL_ROOT/infra-references/querying.md` first, then come back here.

---

## Phase 1: First-Pass Triage

Run these two scripts first, before any open-ended queries. They are cheap
relative to exploration (each takes well under a minute even on a 250 MB heap)
and they answer the two questions a leak investigation always starts with:
*where does the memory live?* and *are destroyed Activities being kept alive?*
Both scripts run against the warm session and return CSV. A trace can contain
**more than one heap dump** (several processes, or one process sampled at
different times); every row carries `process_name` and `graph_sample_ts`, so
handle each dump separately and label your sections by those two values.

1.  **Retention hotspots.**

    ```bash
    trace_processor query --remote SESSION --query-file $SKILL_ROOT/workflows/android_memory/scripts/triage_retention_hotspots.sql
    ```

    Up to 5 rows per dump, ranked by *spread*: the retained bytes of a
    dominator-class-tree node that are not explained by its single largest
    child. Ranking by plain retained size would just return the GC root and
    the funnel of single-child nodes below it; spread finds the node where the
    memory actually branches out. For each hotspot you get its `path` from the
    root (class names and instance counts), `retained_bytes` /
    `retained_pct`, and `top_children` (what it retains, by class).

    Reading it:
    - Large hotspots are not automatically leaks. Ask whether the instance
      count and retained size are plausible for what the class does (a
      cache, a pool, the interned-string table) or whether they look
      unbounded (thousands of copies of a per-request or per-screen object).
    - Common unbounded shapes: a collection or static field that only ever
      grows (`ArrayList`, `HashMap`, a `Class<...>` static holding a list),
      listener/callback lists, and queues of pending work
      (`android.os.Message` chains on a `Looper`, executor
      `LinkedBlockingQueue$Node`s). For queues, `top_children` shows whose
      work items they are.
    - A path whose root is `[MULTIPLE_ROOTS]` is held by more than one GC
      root; linked structures often show up this way.
    - Package prefixes in the path and children (the app's own package vs.
      framework, libraries and third-party SDKs) tell you which component
      owns the retention even without source code.
    - A primitive array (`byte[]`) under `[ROOT_JNI_GLOBAL]` usually means a
      JNI global reference leak.

2.  **Leaked Activities.**

    ```bash
    trace_processor query --remote SESSION --query-file $SKILL_ROOT/workflows/android_memory/scripts/triage_leaked_activities.sql
    ```

    Two result sets. The first counts reachable instances of every
    `android.app.Activity` subclass next to the number of
    `android.view.ViewRootImpl`s (roughly one per live window). Activities
    that are still in the back stack keep their window, so several instances
    of one class can be legitimate (the same screen opened twice). Many more
    Activity instances than `ViewRootImpl`s is the classic signature of
    destroyed Activities being leaked, each pinning its whole view tree.
    The second result set groups
    every Activity instance by the *shape* of its shortest path from a GC
    root (`[ROOT_TYPE] Class .field -> Class .field -> ... -> Activity`), so
    you can say "N of M instances are held by X". The class and field just
    before the Activity (a listener, a lambda's captured `this$0`, a
    coroutine context) is usually the culprit. A path through
    `android.app.ActivityThread .mActivities` is the framework's own record
    of a live Activity and is normal; one instance per class held that way
    is not a leak. Processes without UI (e.g. `system_server`) have no
    Activities, so empty result sets are expected there.

3.  **Connect the two.** When both scripts point at the same objects (a
    hotspot's path or children also appear in the Activity GC-root paths),
    they are describing one leak from two ends; say so. When they point at
    different things, report them as separate findings.
    If nothing looks abnormal (top hotspots are things every process has,
    such as interned strings or ICU caches, and no duplicate Activities),
    say there is no clear leak signal. One snapshot cannot rule out a slow
    leak; suggest a second dump and
    [heap_dump_diff.md]($SKILL_ROOT/workflows/android_memory/heap_dump_diff.md).

4.  **Source code.** If the app's source is available (search the workspace
    for the process name and the app classes in the paths), use it to
    confirm the hypothesis and point at specific code. If it is not, which is
    the common case for third-party apps, **do not stop**: the class names,
    field names, instance counts and package prefixes from the scripts are
    enough to name the responsible components and propose concrete fixes.
    Label such conclusions as inferred from the heap graph rather than
    confirmed in source.

5.  **When to go further.** Continue into Phase 2 on your own when Phase 1
    leaves the main question open, for example: you can't tell who holds a
    hotspot (expand it with the reference queries in Phase 2, Step 4), a
    hotspot is a generic collection (`Object[]`, `HashMap$Node[]`) whose
    owner isn't clear from the path, or the user asked for something Phase 1
    doesn't cover. Keep it targeted: a few queries to answer a specific
    question, not a full re-exploration.

> [!NOTE]
> `scripts/triage_dominator_path.sql` (the single largest dominated path per
> dump) is still used by `heap_dump_cluster.md` for fleet clustering. For a
> single dump, the two scripts above supersede it.

---

## Phase 2: Open-Ended Exploratory Deep Dive

Use this when Phase 1 leaves a specific question open (see Phase 1, step 5)
or the user asks for deeper analysis.

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
get a human-readable retention path. Beware linked structures
(`android.os.Message.next`, `LinkedBlockingQueue$Node`): their chains can be
100k+ objects deep, so recursive walks along them are very slow. Read the
`dominated_*` columns of the head object instead of walking the list.

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

Whether reporting on Phase 1 (First-Pass Triage) or Phase 2 (Exploratory Deep
Dive), a high-quality summary for the user contains:

1.  **Orienting Context:** The process name and timestamp of the heap dump.
2.  **Primary Leak Signatures:** The retention hotspots with their retained
    sizes, and any Activity classes with more live instances than expected.
3.  **Retention Chains:** For each suspected leak, at least one concrete chain
    from the GC root to the leaked object, with class and field names (the
    `gc_root_path` from `triage_leaked_activities.sql` or the hotspot `path`).
    This is what lets the developer find the offending reference in their
    code, so include it without being asked.
4.  **Hypothesis & Architectural Recommendations:**
    - If the app's source is available, use it to confirm the lifecycle and
      ownership of the suspected classes and cite specific locations.
    - If it is not, build the hypothesis from the heap graph (field names,
      captured `this$0` / `$cont` references, package prefixes, instance
      counts) and say that it is inferred, not confirmed in source.
    - If `class_name` is a primitive array like `byte[]` and the path begins
      with `[ROOT_JNI_GLOBAL]`, advise the user of a likely JNI global reference
      leak.
    - Provide concrete recommendations (e.g., cancelling pending callbacks or
      tasks when a coroutine/lifecycle ends, clearing listeners in
      `onDestroy`, avoiding static references to Activities/Contexts, bounding
      queues).
5.  **Actionable Implementation Plan:** Reference specific code locations
    (filenames and line numbers) where source is available, otherwise the
    classes and fields to look for, and a concrete plan for fixing or
    mitigating the leak.

Always keep the underlying SQL and object IDs available so the user can audit.
Keep measured facts (counts, sizes, reference chains) clearly separate from
inferred explanations of what the code is doing.

## Reference

- Java heap profiler:
  <https://perfetto.dev/docs/data-sources/java-heap-profiler>
- PerfettoSQL language tour:
  <https://perfetto.dev/docs/analysis/perfetto-sql-getting-started>
- Generated stdlib reference: <https://perfetto.dev/docs/analysis/stdlib-docs>
