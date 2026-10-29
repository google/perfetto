# Diffing Android Java Heap Dumps

Compares two Android Java heap dumps (before vs after), either Perfetto
`java_heap_profiler` traces or Android `.hprof` dumps.

## Step 1: Load both dumps into warm sessions

Load each dump once into a named session so every query reuses the parsed heap
graph instead of re-parsing the file. Load them in parallel:

```bash
trace_processor server unix --name before_tp --daemonize <before_dump> &
trace_processor server unix --name after_tp --daemonize <after_dump> &
wait
```

A large heap dump can take about a minute to load.

Kill both sessions when finished:
`trace_processor server kill before_tp && trace_processor server kill after_tp`.

## Step 2: Find which classes grew

Diff the two sessions:

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/diff_heap_dumps.py before_tp after_tp
```

The script queries `android_heap_graph_stats` and
`android_heap_graph_class_aggregation` in both sessions, then reports the diffed
snapshots, the heap summary (with reachable Java heap bytes and reachable native
bytes on separate rows), **Top Application Classes by Dominated Bytes
Growth**, **Top Application Classes by Instance Count Growth**, **Application
Classes Added or Removed (0 -> N or N -> 0)**, and **Top
Libcore / Array Classes by Dominated Bytes Growth** (arrays and `libcore` types
are listed separately because they are usually the payload of an app-class leak,
not its cause). The first run builds the dominator tree in both sessions,
which takes about a minute on a large heap; later queries on the same sessions
reuse it.

By default, the first matching snapshot in each session is compared. If a
session holds multiple snapshots or processes, select specific ones with
`--before-upid` / `--after-upid` and `--before-ts` / `--after-ts` (the script
lists the available ones when a session holds several processes). Pass
`--top N` to show more rows per table (default 10), and
`--class-glob '<pattern>'` (repeatable) to filter to specific packages:

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/diff_heap_dumps.py \
  before_tp after_tp --before-upid <BEFORE_UPID> --after-upid <AFTER_UPID> \
  --top 30 --class-glob 'com.example.*'
```

Total heap size fluctuates on its own (e.g. caches, `SoftReference` eviction),
so never dismiss a regression because the heap total looks flat.

- **Check whether growth scales with repeated actions.** If an action ran N
  times between the dumps and a class gained about N instances (or a multiple
  of N), each run left objects behind — a leak. If counts only grew by a small
  constant (such as `+1`) after N runs, the action is not leaking on every run;
  instead, one extra object is staying alive (for example, an extra window,
  screen, or cache entry). If the action ran only once, counts alone cannot
  tell a leak from normal live state, so check whether those objects should
  still be alive after the action finishes.
- **Look past the top 10 when searching for `+1` root objects.** One extra
  high-level object (such as `+1` `Activity`, window, or controller) often
  brings in dozens of child `View`s and strings that fill the default top-10
  table and push the `+1` class out of view. Re-run `diff_heap_dumps.py` with a
  larger `--top` and `--class-glob` for the relevant packages so you can see
  all the classes that grew together, and report their `before -> after`
  counts.
- **Check the added and removed (`0 -> N` / `N -> 0`) table.** Classes that
  appear (`0 -> N`) while related classes disappear (`N -> 0`) often come from
  a code, flag, or library change between the two runs. Check in Step 3 whether
  the new classes appear on the retention paths of the objects that grew.

## Step 3: Inspect the classes that grew

Step 2 usually flags several related classes (for example, a leaked object, its
inner classes, and the arrays holding them) that belong to the same leak, so
inspect them together.

### 3.1 What keeps them alive? (Retention paths)

When the growth is in `Activity` instances or a single large hotspot, run the
Phase 1 triage scripts from
[heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md)
(`triage_retention_hotspots.sql`, `triage_leaked_activities.sql`) on
`after_tp`. To find what holds a specific class from Step 2 alive, fill in
`<class_name>` in `explain_retention.sql` and run it on `after_tp` (single
quotes prevent the shell from expanding `$` in inner class names):

```bash
sed 's/<class_name>/com.example.LeakedThing$Inner/' \
  $SKILL_ROOT/workflows/android_memory/scripts/explain_retention.sql |
  trace_processor query --remote after_tp -f -
```

The query groups reachable instances of the class by their shortest
strong-reference path from a GC root and returns up to 20 paths per dump,
ordered by total bytes (`java_bytes + native_bytes`). Each row reports
`instances`, `java_bytes`, `native_bytes`, `example_object_id`, and
`gc_root_path` (`[ROOT_TYPE] RootClass .field -> ... -> TargetClass`, with
repeated same-class hops collapsed). If the class already had instances in
`before_tp`, run the same query on `before_tp` to see which path grew.

Start with the main application class that grew (if an action ran N times, pick
a class whose count grew by about N), skipping generic containers like
collections, arrays, and lambdas. The first run in a session builds the stdlib
path tables and takes a few seconds; later runs take under a second.

> [!CAUTION] Do not write custom recursive queries or multi-hop self-joins
> (`r1 JOIN r2 ON r2.owned_id = r1.owner_id`) over `heap_graph_reference`, and
> do not walk the heap graph from the roots down. `heap_graph_reference` has
> millions of cyclic edges, so those queries take minutes or time out. If you
> need a custom query, walk up `heap_graph_dominator_tree.idom_id` or
> `_heap_graph_object_min_depth_tree.parent_id` and join `heap_graph_reference`
> through `heap_graph_object.reference_set_id`.

### 3.2 Which instances leaked? (`.hprof` only)

Step 2 and 3.1 treat every instance of a class as interchangeable, yet the
leaked instances and the legitimate ones are usually identical in everything
they expose: same class, same size, same retention path. Only their field values
differ. Reading those values tells you *which* objects were kept, turning, say,
"`ImageRequest` grew by 30 instances" into a reproducible trigger ("every
preview request is retained").

Grouping the reachable instances by field value makes the leaked configuration
(e.g. `is_preview_request = true`) stand out from the baseline population. Run
the queries on both `before_tp` and `after_tp`: the values whose counts grew
identify the leak, and the ones that stayed flat confirm the rest of the
population is healthy.

Field values are only recorded in Android `.hprof` dumps; Perfetto
`java_heap_profiler` traces store the object graph without them, so these
queries return nothing for them.

Values live in two places, so there is one query for each. Use `-f - << 'EOF'`
so `$` in inner class names (e.g. `Foo$Bar`) is not expanded by the shell.

**String fields** are ordinary references: the owner points at a
`java.lang.String` instance through `heap_graph_reference`, and that instance
carries the text in `heap_graph_object_data.value_string`.

```bash
trace_processor query --remote after_tp -f - << 'EOF'
    SELECT r.field_name, d.value_string, COUNT(*) AS instances
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    JOIN heap_graph_reference r ON o.id = r.owner_id
    JOIN heap_graph_object s ON r.owned_id = s.id
    JOIN heap_graph_object_data d ON s.object_data_id = d.id
    WHERE o.reachable = 1
      AND COALESCE(c.deobfuscated_name, c.name) = '<LEAKED_CLASS>'
      AND d.value_string IS NOT NULL
    GROUP BY 1, 2
    ORDER BY instances DESC;
EOF
```

**Primitive fields** hang off the owner itself: `heap_graph_object.object_data_id`
leads to `heap_graph_primitive`, which has one nullable column per primitive type
(`int_value`, `bool_value`, ...) and sets exactly one of them per row, hence the
`COALESCE`.

```bash
trace_processor query --remote after_tp -f - << 'EOF'
    SELECT
      p.field_name,
      p.field_type,
      COALESCE(p.bool_value, p.byte_value, p.char_value, p.short_value,
               p.int_value, p.long_value, p.float_value, p.double_value) AS value,
      COUNT(*) AS instances
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    JOIN heap_graph_object_data d ON o.object_data_id = d.id
    JOIN heap_graph_primitive p ON d.field_set_id = p.field_set_id
    WHERE o.reachable = 1
      AND COALESCE(c.deobfuscated_name, c.name) = '<LEAKED_CLASS>'
      -- Exclude ART's synthetic Object lock-word header (shadow$_monitor_).
      AND p.field_name NOT GLOB 'java.lang.Object.shadow*'
    GROUP BY 1, 2, 3
    ORDER BY instances DESC;
EOF
```

Read the results top-down: a value shared by many instances is a configuration,
and comparing its count between the two sessions tells you whether that
configuration is the one leaking. Identity fields (e.g. `android.os.Binder.mObject`
pointers, hashes) differ per instance and pile up as `instances = 1` rows at the
bottom; ignore them.

### 3.3 Bitmaps (native pixel bytes)

Since Android 8.0, `android.graphics.Bitmap` stores pixel data in native
memory. The Java object's `self_size` is only a small header; the pixel byte
size is in `heap_graph_object.native_size`. Group reachable Bitmaps by
`native_size` in both sessions to see which Bitmap sizes grew:

```bash
trace_processor query --remote after_tp -f - << 'EOF'
    SELECT o.native_size, COUNT(*) AS bitmaps, SUM(o.native_size) AS native_bytes
    FROM heap_graph_object o
    INNER JOIN heap_graph_class c ON o.type_id = c.id
    WHERE o.reachable = 1 AND c.name = 'android.graphics.Bitmap'
    GROUP BY 1
    ORDER BY native_bytes DESC;
EOF
```

Then run `explain_retention.sql` with `<class_name>` set to
`android.graphics.Bitmap` on `after_tp` (and `before_tp` if needed) — paths are
ordered by `java_bytes + native_bytes`, so the largest native Bitmap holders
appear first. If a leaked app object holds the Bitmap, report the Bitmap growth
as part of that object's leak rather than a separate Bitmap leak. If a live UI
component holds it (for example, a list item, widget, or dialog), the Bitmap is
that component's image — check why that component is present.

Compare Bitmap count and native bytes together:

| Bitmap count | Bitmap native bytes | Typical cause |
| :--- | :--- | :--- |
| up | up proportionally | More Bitmaps kept alive |
| flat | up | Larger Bitmaps (higher resolution, density, scaling, or `Bitmap.Config` change) |
| up | flat | More, smaller Bitmaps |

## Further investigation and reporting

Once you have identified the leaked classes and their retention paths, follow
Phase 2 Step 4 (inspecting `heap_graph_reference` holders) and Phase 3 (code
search & fix plan) in
[heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md). When the
growth is one extra live object rather than a leak that grows on every run,
check what differed between the two runs; if system traces of both runs are
available, use
[system_trace_memory.md]($SKILL_ROOT/workflows/android_memory/system_trace_memory.md)
to compare their memory counters and running processes.

## Reporting and query rules

1.  Report before and after counts with the delta (e.g. `12 -> 42 (+30 instances)`),
    the retained (`dominated_bytes`) delta, Java and native byte deltas
    separately, and the retention path holding the objects.
2.  Take class sizes from `android_heap_graph_class_aggregation`. Summing
    `dominated_size_bytes` across instances of a self-nesting class (e.g. a
    `View` inside a `View`) double-counts inner instances.
3.  If Step 2 reported several snapshots in a trace, scope custom queries with
    `o.upid = <upid> AND o.graph_sample_ts = <ts>`. Do not filter on
    `process.name`: in `.hprof` dumps `process.name` is `NULL`.
