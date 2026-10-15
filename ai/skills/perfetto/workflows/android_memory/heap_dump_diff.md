# Diffing Android Java Heap Dumps

Compares two Android Java heap dumps (before vs after), either Perfetto
`java_heap_profiler` traces or Android `.hprof` dumps.

## Step 1: Load both dumps into warm sessions

Load each dump once into a named session so every query reuses the parsed heap
graph instead of re-parsing the file:

```bash
trace_processor server unix --name before_tp --daemonize <before_dump>
trace_processor server unix --name after_tp --daemonize <after_dump>
```

Kill both sessions when finished:
`trace_processor server kill before_tp && trace_processor server kill after_tp`.

## Step 2: Find which classes grew

Diff the two sessions:

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/diff_heap_dumps.py before_tp after_tp
```

The script queries `android_heap_graph_stats` and
`android_heap_graph_class_aggregation` in both sessions, then reports the diffed
snapshots, the heap summary, **Top Application Classes by Dominated Bytes
Growth**, **Top Application Classes by Instance Count Growth**, and **Top
Libcore / Array Classes by Dominated Bytes Growth** (arrays and `libcore` types
are listed separately because they are usually the payload of an app-class leak,
not its cause).

By default, the first matching snapshot in each session is compared. If a
session holds multiple snapshots or processes, select specific ones with
`--before-upid` / `--after-upid` and `--before-ts` / `--after-ts`.

Total heap size fluctuates on its own (e.g. caches, `SoftReference` eviction),
so never dismiss a regression because the heap total looks flat.

## Step 3: Inspect the classes that grew

Step 2 usually flags several related classes (e.g. a leaked object, its inner
classes, and the arrays holding them). They are facets of the same leak, so
investigate them together, running the queries below once per class as
`<LEAKED_CLASS>`.

### 3.1 What keeps them alive? (Dominator retention paths)

Follow Phase 2 Step 3 (*Use the dominator tree to find the retention chain*) and
Step 4 (*Inspect specific references when needed*) in
[heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md) against
`after_tp` to walk `idom_id` in `heap_graph_dominator_tree` from each class up to
its GC root (`root_type`), and join `heap_graph_reference` (`owner_id`,
`owned_id`, `field_name`) to identify the exact field holding each edge.

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

## Further investigation and reporting

Once you have identified the leaked classes and their retention paths, follow
Phase 2 Step 4 (inspecting `heap_graph_reference` holders) and Phase 3 (code
search & fix plan) in
[heap_dump.md]($SKILL_ROOT/workflows/android_memory/heap_dump.md).

## Reporting and query rules

1.  Report before and after counts with the delta (e.g. `12 -> 42 (+30 instances)`),
    the retained (`dominated_bytes`) delta, and the retention path holding the
    objects.
2.  Take class sizes from `android_heap_graph_class_aggregation`. Summing
    `dominated_size_bytes` across instances of a self-nesting class (e.g. a
    `View` inside a `View`) double-counts inner instances.
3.  If Step 2 reported several snapshots in a trace, scope custom queries with
    `o.upid = <upid> AND o.graph_sample_ts = <ts>`. Do not filter on
    `process.name`: in `.hprof` dumps `process.name` is `NULL`.
