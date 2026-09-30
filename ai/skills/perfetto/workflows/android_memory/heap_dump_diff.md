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

A heap dump of a large app (15-20 MB) takes 30-60 seconds to load. Wait for
both commands to return; killing and restarting a session that is still
loading only starts the parse over.

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
bytes as separate rows), **Top Application Classes by Dominated Bytes
Growth**, **Top Application Classes by Instance Count Growth**, **Application
Classes Added or Removed (0 -> N or N -> 0)**, and **Top
Libcore / Array Classes by Dominated Bytes Growth** (arrays and `libcore` types
are listed separately because they are usually the payload of an app-class leak,
not its cause). On a large heap the first run takes one to two minutes to
materialize the dominator tree in both sessions; later queries on the same
sessions reuse it in seconds.

By default, the first matching snapshot in each session is compared. If a
session holds multiple snapshots or processes, select specific ones with
`--before-upid` / `--after-upid` and `--before-ts` / `--after-ts`. Pass
`--top N` to widen the application class tables (default 10), and
`--class-glob 'com.example.*'` (repeatable) to limit them to specific packages.

Total heap size fluctuates on its own (e.g. caches, `SoftReference` eviction),
so never dismiss a regression because the heap total looks flat.

- **Growth proportional to the repetitions is a leak.** If the scenario
  repeated an action N times, a class that gained about N instances (or a
  multiple of N) leaks once per repetition. A fixed gain of one top-level
  object and its children is a one-time state change, such as one extra
  window, notification, or cached screen, not a per-action leak.
- **Census a step change before explaining it.** The top-N tables cut off the
  small `+1` classes that show what the extra unit is. Re-run the script with a
  larger `--top` and a `--class-glob` for the unit's package or domain noun, and
  report `before -> after` for every class that grew with it.
- **Added and removed classes reveal implementation swaps.** When a library or
  feature flag replaces one implementation with another, the old classes go
  `N -> 0` while the new ones go `0 -> N`. Treat the swap as the prime suspect
  when the retention paths from Step 3 run through the new classes.
- **Dominator reshuffling is not growth.** A singleton (`1 (unchanged)`) whose
  dominated bytes jumped may just have become the sole dominator of an object
  that already existed, because another reference to it went away. Check that
  the dominated object (e.g. a Bitmap of identical native size) is new before
  calling the singleton a leak.

## Step 3: Inspect the classes that grew

Step 2 usually flags several related classes (e.g. a leaked object, its inner
classes, and the arrays holding them). They are facets of the same leak, so
investigate them together.

### 3.1 What keeps them alive? (Dominator retention paths)

Explain the retention of the most telling classes in one run:

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/explain_retention.py after_tp \
  --class '<GROWING_CLASS>' --class '<NEW_CLASS>'
```

For each class, the script prints:

1. **the holder groups**: its reachable instances grouped by their two nearest
   dominators. Old and new instances of a grown class are often held by
   different owners (e.g. 2 old instances in a long-lived cache, 40 new ones in
   leaked objects), so an arbitrary instance can explain the wrong population.
   The script traces an instance of the largest group; trace a smaller group
   with `--object-id <example object>` when its count matches the growth
   instead.
2. **the dominator chain**, from the GC root down, with the field through which
   each dominator holds the next object and the bytes it retains. The top rows
   name the retaining container. The field is empty where several paths
   converge on an object.
3. **the shortest strong reference path from a GC root**, computed with
   `graph_reachable_bfs!` and ignoring `Reference.referent` edges. It names every
   field on the way, including those the dominator chain skips.

The dominator chain takes about a minute on a large heap, and the path search
takes from 20 seconds to 3 minutes. Pass every target in one run, because one
graph search serves all of them. Use `--dominators-only` to skip the path
search when the chain already names the retaining field. Pass `--upid` /
`--ts` when the session holds several dumps, and `--object-id` to trace a
specific instance.

Pick the targets from Step 2:

1. the top growing application class whose gain scales with the repetitions,
   skipping collections, arrays, and lambdas, which are containers rather than
   the leaked objects;
2. if Step 2 shows an implementation swap, also the top added (`0 -> N`) class
   that scales with the repetitions, to see whether its retention runs through
   the new implementation.

> [!CAUTION] Do not hand-write recursive queries over `heap_graph_reference`.
> It holds millions of cyclic edges: a top-down `WITH RECURSIVE` from the GC
> roots, a self-join, or a lookup without an `owner_id = <id>` or
> `owned_id = <id>` filter runs until the caller gives up. For a one-hop look at
> a specific object's fields, use Phase 2 Step 4 of
> [heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md).

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

### 3.3 Bitmaps: the count is Java, the bytes are native

Since Android 8.0 a `android.graphics.Bitmap` keeps its pixels in native memory
and only references them from the Java object. Its `self_size` stays small
whatever the image size, and the pixel bytes are in
`heap_graph_object.native_size`. Group the reachable Bitmaps by native size in
both sessions to find the size bucket that grew:

```bash
trace_processor query --remote after_tp -f - << 'EOF'
    SELECT o.native_size, COUNT(*) AS bitmaps, SUM(o.native_size) AS native_bytes
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    WHERE o.reachable = 1 AND c.name = 'android.graphics.Bitmap'
    GROUP BY 1
    ORDER BY native_bytes DESC;
EOF
```

Then explain the bucket that grew with `explain_retention.py --class
android.graphics.Bitmap --native-size <native_size>`, and compare its holder
groups with the bucket's growth. If a leaked application object dominates the
Bitmap, the Bitmap growth is a consequence of that leak, not a separate one.
If a newly added UI element dominates it (for example, a notification row, an
app widget, a list item, or a dialog), the Bitmaps are that element's content:
explain why the element appeared rather than looking for a Bitmap leak.

Read the count and the bytes together:

| Bitmap count | Bitmap native bytes | Most likely cause                         |
| :----------- | :------------------ | :---------------------------------------- |
| up           | up proportionally   | more Bitmaps held alive                   |
| flat         | up                  | bigger Bitmaps: density, resolution,      |
:              :                     : scaling or `Bitmap.Config` change         :
| up           | flat                | more but smaller Bitmaps, often a caching |
:              :                     : or decode-path change                     :

## Further investigation and reporting

Once you have identified the leaked classes and their retention paths, follow
Phase 2 Step 4 (inspecting `heap_graph_reference` holders) and Phase 3 (code
search & fix plan) in
[heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md). For a constant
`+1` step change (for example, one extra window, notification, or cached
screen), look for what differed between the two runs before blaming a
per-action leak.

When Step 2 showed an implementation swap and the retention paths run through
the new implementation, report the new implementation, the field in it that
keeps the leaked objects alive, and the flag or change that enabled it.
Describe the mechanism only as far as the retention paths and the source code
support it.

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
