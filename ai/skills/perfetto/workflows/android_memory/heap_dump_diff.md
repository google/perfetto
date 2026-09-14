# Diffing Android Java Heap Dumps

This workflow compares two Android Java heap dumps (`before_trace` vs
`after_trace`) captured with Perfetto's `java_heap_profiler` data source or
Android `.hprof` dumps.

## Entry Point: Choose Your Starting Step

-   **If you DO NOT know which class leaked** (e.g., total Java heap size or
    overall object count increased): Start at **Step 1** (`diff_heap_graphs.py`)
    to discover the top regressed classes and dominator paths across the
    process.
-   **If you ALREADY know which class or base type leaked** (e.g., a metric for
    a specific class like `android.graphics.Bitmap`, `android.app.Activity`,
    `android.view.View`, or `android.os.Binder` regressed):
    -   Pass `--class_filter <ClassName>` to `diff_heap_graphs.py` in **Step 1**
        to view the before/after delta and dominator paths exclusively for that
        class, **OR**
    -   Jump directly to **Step 2** to inspect incoming holders, subclasses, or
        custom `.hprof` field values (`String` / primitive fields) for `<ClassName>`.

## Step 1: Discover Target Process & Run Automated Heap Graph Diff (`diff_heap_graphs.py`)

### Step 1.1: Discover Available Processes & Heap Dumps

Before running the diff script, discover the exact process name (`process_name`)
and available heap dump timestamps (`graph_sample_ts`) recorded in the trace
(especially important for `.hprof` files where `process_name` is often `pid=0`
or when multiple processes/dumps were captured):

```bash
trace_processor <before_trace> -Q "
SELECT DISTINCT
  COALESCE(p.name, 'pid=' || p.pid) AS process_name,
  g.graph_sample_ts
FROM heap_graph_object g
JOIN process p USING (upid)
ORDER BY g.graph_sample_ts;
"
```

### Step 1.2: Run `diff_heap_graphs.py`

Run the bundled Python diffing script against the baseline (`before_trace`) and
candidate (`after_trace`) `.perfetto-trace` or `.hprof` files using the
discovered `--process_name` (optionally adding `--class_filter <ClassName>` if
the regressed class is already known):

```bash
python3 $SKILL_ROOT/workflows/android_memory/scripts/diff_heap_graphs.py \
  --process_name <process_name> \
  --before_trace <path/to/before.perfetto-trace> \
  --after_trace <path/to/after.perfetto-trace> \
  [--class_filter <ClassName>] \
  [--sort_by dominated_size|reachable_count|reachable_size|native_size] \
  [--before_index <idx> --after_index <idx>] \
  [--before_ts <graph_sample_ts> --after_ts <graph_sample_ts>] \
  [--output_file <report.md>] \
  --top_n 10
```

-   **Filtering by Class Name**:
    -   `--class_filter <ClassName>`: Restricts the class delta and dominator
        path tables to classes containing `<ClassName>` (case-insensitive).
-   **Choosing the Ranking Metric (`--sort_by`, default `dominated_size`)**:
    -   `dominated_size`: retained bytes. Best for one big leaked object graph.
    -   `reachable_count`: instance count. Best for leaks of many small objects
        (listeners, callbacks, or IPC stubs) that barely move the byte totals.
        The report always prints an instance-count table as well, so switching
        is only needed to reorder the primary table.
    -   `reachable_size`: shallow bytes. Useful when retained size
        (`dominated_size`) is distorted by shared containers or objects
        referenced by multiple holders.
    -   `native_size`: `NativeAllocationRegistry` bytes. Use for `Bitmap`,
        `Surface` and other Java wrappers over native buffers, whose Java
        shallow size barely changes while native memory grows.
-   **Disambiguating Multiple Heap Dumps in a Trace**:
    -   A single Perfetto trace can contain multiple heap dumps recorded at
        different `graph_sample_ts` timestamps (e.g., periodic continuous dumps
        or start/end dumps in one trace).
    -   By default, when comparing two different trace files, the script selects
        the **latest** dump (`index = -1`) in each trace and lists all available
        `graph_sample_ts` timestamps in the report header.
    -   When comparing two heap dumps **within the same trace file**
        (`--before_trace trace.pftrace --after_trace trace.pftrace`), the script
        automatically compares the **first** dump (`--before_index 0`) against
        the **last** dump (`--after_index -1`). You can also explicitly select
        any dump by 0-based index (`--before_index` / `--after_index`) or exact
        timestamp (`--before_ts` / `--after_ts`).

### Interpreting the Report

The script automatically executes `query_class_aggregation.sql`,
`query_dominator_class_paths.sql`, and `query_class_field_values.sql` in a
single `trace_processor` pass per trace and outputs up to six sections:

1.  **Process Macro Overview**: Total reachable shallow heap bytes and object
    counts for `<process_name>` at the selected `graph_sample_ts`.
    > [!IMPORTANT]
    > Total reachable heap size can fluctuate or decrease slightly between runs
    > due to run-to-run variance in unrelated live caches (`LruCache`),
    > `SoftReference` eviction, or background tasks. A leak of small objects
    > (e.g., +50 listener or callback instances totaling ~2 KB) is easily masked
    > by a 100 KB fluctuation in unrelated live caches—never dismiss a
    > regression based solely on macro reachable heap size delta.
2.  **Top Regressed Classes (Sorted by `--sort_by`, default `dominated_size`)**:
    Classes with the largest increase in the selected ranking metric (by
    default, total retained memory `dominated_size_bytes +
    dominated_native_size_bytes`, or shallow bytes / instance count / native
    bytes when configured via `--sort_by`).
3.  **Top Regressed Classes by Instance Count**: Highlights small wrapper
    objects, listeners, callbacks, or IPC stubs whose instance counts increased
    significantly even if each instance is small.
4.  **Top Regressed Dominator Class Paths**: Exact GC root retention chains
    (`[ROOT] -> Class (fieldName) -> ... -> Class`) whose dominated self-size or
    instance count increased the most, including the exact holding field names
    along each dominator edge.
5.  **Top Regressed Field Values (`String` & Primitive Fields)**: Automatically
    included when `.hprof` field values are present. Shows which specific
    `String` or primitive field values (e.g., `tileSpec='bluetooth_audio'`,
    `refreshIntervalMs=1500`) grew in instance count or dominated size.
6.  **Top Regressed Instance Configurations (Field Value Tuples)**: Automatically
    included when `.hprof` field values are present. Groups leaked instances by
    their complete tuple of `String` and primitive field values so you can
    immediately see the exact configuration of the leaked objects without manual
    SQL queries.

> **When to Stop**: If the Top Regressed Classes, Dominator Class Paths, and
> Field Value / Instance Configuration tables clearly identify the leaked class,
> its holding reference chain, and its field values, **stop here** and inspect
> the application source code. Only run the targeted queries in Step 2 if the
> root cause remains ambiguous.

## Step 2: Conditional Follow-Up Queries (Use Only When Needed)

Choose the specific query below based on why Step 1 was inconclusive:

-   **If you need to aggregate all subclasses of a base class or interface
    (e.g., `android.view.View`, `android.app.Activity`, or `android.os.Binder`),
    or inspect 1-hop incoming/outgoing references**:
    `android_heap_graph_class_aggregation` groups strictly by exact concrete
    class name rather than summing polymorphic subclasses, and dominator paths
    do not show non-dominating incoming references or what fields a wrapper
    holds. Jump to **Case A: Polymorphic Subclass Aggregation & 1-Hop Reference
    Queries**.
-   **If the leaked class is a generic container (`Bundle`, `HashMap$Node`,
    `PendingIntent`, or shared pool) and `.hprof` files are available**:
    Structural class names do not show which feature or caller created the
    entries. Jump to **Case B: Field Value Attribution Queries (`.hprof`
    Only)**.

### Case A: Polymorphic Subclass Aggregation & 1-Hop Reference Queries

1.  **Count All Subclasses of a Base Class (e.g. `android.view.View`, `android.app.Activity`, `android.os.Binder`)**:
    -   `android_heap_graph_class_aggregation` groups strictly by exact concrete `type_name`. To count all instances assignable to a base class or interface across the class hierarchy (for example, all `android.view.View`, `android.app.Activity`, or `android.os.Binder` subclasses), use recursive `superclass_id` traversal on `heap_graph_class`.

```sql
WITH RECURSIVE target_subclasses AS (
  SELECT id, COALESCE(deobfuscated_name, name) AS class_name
  FROM heap_graph_class
  -- Replace with 'android.view.View', 'android.app.Activity', or 'android.os.Binder'
  WHERE COALESCE(deobfuscated_name, name) = 'android.os.Binder'
  UNION ALL
  SELECT c.id, COALESCE(c.deobfuscated_name, c.name) AS class_name
  FROM heap_graph_class c
  JOIN target_subclasses ts ON c.superclass_id = ts.id
)
SELECT
  COALESCE(pr.name, 'pid=' || pr.pid) AS process_name,
  ts.class_name,
  COUNT(*) AS reachable_obj_count,
  SUM(o.self_size) AS shallow_bytes
FROM heap_graph_object o
JOIN target_subclasses ts ON o.type_id = ts.id
JOIN process pr ON o.upid = pr.id
WHERE o.reachable = 1
  AND pr.name = '<process_name>'
GROUP BY process_name, ts.class_name
ORDER BY reachable_obj_count DESC
LIMIT 25;
```

2.  **Inspect Outgoing & Incoming 1-Hop References**:

```sql
-- Outgoing references FROM a root/wrapper class (e.g. what callback field does ContentObserver$Transport wrap?)
SELECT
  COALESCE(rc.deobfuscated_name, rc.name) AS target_class,
  COALESCE(r.deobfuscated_field_name, r.field_name) AS field_name,
  COUNT(*) AS instance_count
FROM heap_graph_object oo
JOIN heap_graph_class oc ON oo.type_id = oc.id
JOIN heap_graph_reference r ON oo.id = r.owner_id
JOIN heap_graph_object ro ON r.owned_id = ro.id
JOIN heap_graph_class rc ON ro.type_id = rc.id
JOIN process pr ON oo.upid = pr.id
WHERE oo.reachable = 1
  AND pr.name = '<process_name>'
  AND COALESCE(oc.deobfuscated_name, oc.name) = '<WRAPPER_OR_BINDER_CLASS>'
GROUP BY target_class, field_name
ORDER BY instance_count DESC;

-- External incoming references TO a leaked class (what holder object retains <LEAKED_CLASS>?)
SELECT
  COALESCE(oc.deobfuscated_name, oc.name) AS holder_class,
  COALESCE(r.deobfuscated_field_name, r.field_name) AS field_name,
  COUNT(*) AS instance_count
FROM heap_graph_object ro
JOIN heap_graph_class rc ON ro.type_id = rc.id
JOIN heap_graph_reference r ON ro.id = r.owned_id
JOIN heap_graph_object oo ON r.owner_id = oo.id
JOIN heap_graph_class oc ON oo.type_id = oc.id
JOIN process pr ON ro.upid = pr.id
WHERE ro.reachable = 1
  AND oo.reachable = 1
  AND pr.name = '<process_name>'
  AND COALESCE(rc.deobfuscated_name, rc.name) = '<LEAKED_CLASS>'
  AND COALESCE(oc.deobfuscated_name, oc.name) NOT LIKE '<LEAKED_CLASS>%'
GROUP BY holder_class, field_name
ORDER BY instance_count DESC;
```

### Case B: Field Value Attribution Queries (`.hprof` Only)

> [!IMPORTANT]
> Field values (`String` contents and primitive values) exist **only** in
> Android `.hprof` heap dumps (`am dumpheap`), not in Perfetto
> `java_heap_profiler` traces (`.perfetto-trace`). To inspect them, load an
> `.hprof` file into `trace_processor`:
>
> ```bash
> trace_processor -q query.sql dump.hprof
> ```

When given an `.hprof` file, Perfetto `trace_processor` exposes:
-   `heap_graph_object_data.value_string`: Decoded `java.lang.String` values
    referenced by object fields (`heap_graph_reference.owner_id -> owned_id`).
-   `heap_graph_reference`: Edges between objects (`owner_id -> owned_id`), with
    field names (`deobfuscated_field_name` / `field_name`).
-   `heap_graph_primitive`: Primitive field values (`boolean`, `int`, `long`,
    `byte`, `short`, `char`, `float`, `double`) joined via
    `heap_graph_object_data.field_set_id`.

#### 1. Query Direct `String` Fields on a Class (or Class Substring)

Inspect all `String` field values referenced by instances of a class:

```sql
INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;

SELECT
  COALESCE(c.deobfuscated_name, c.name) AS class_name,
  COALESCE(r.deobfuscated_field_name, r.field_name) AS field_name,
  od.value_string AS string_value,
  COUNT(*) AS instance_count,
  SUM(o.self_size) AS total_shallow_bytes,
  SUM(COALESCE(dt.dominated_size_bytes, o.self_size)) AS total_dominated_bytes
FROM heap_graph_object o
JOIN heap_graph_class c ON o.type_id = c.id
JOIN heap_graph_reference r ON o.id = r.owner_id
JOIN heap_graph_object str_obj ON r.owned_id = str_obj.id
JOIN heap_graph_object_data od ON str_obj.object_data_id = od.id
LEFT JOIN heap_graph_dominator_tree dt ON o.id = dt.id
WHERE o.reachable = 1
  AND COALESCE(c.deobfuscated_name, c.name) LIKE '%<CLASS_NAME>%'
  AND od.value_string IS NOT NULL
GROUP BY class_name, field_name, string_value
ORDER BY instance_count DESC, total_dominated_bytes DESC
LIMIT 50;
```

#### 2. Reverse Lookup: Find Objects & Classes Holding a Known String Value

When you know a keyword, URL, tag, or action string (e.g. `'bluetooth'`, `'notification'`),
find which classes and fields reference that string across the entire heap:

```sql
SELECT
  COALESCE(holder_class.deobfuscated_name, holder_class.name) AS holder_class_name,
  COALESCE(r.deobfuscated_field_name, r.field_name) AS field_name,
  od.value_string AS matched_string,
  COUNT(*) AS reference_count
FROM heap_graph_object_data od
JOIN heap_graph_object str_obj ON od.id = str_obj.object_data_id
JOIN heap_graph_reference r ON str_obj.id = r.owned_id
JOIN heap_graph_object holder_obj ON r.owner_id = holder_obj.id
JOIN heap_graph_class holder_class ON holder_obj.type_id = holder_class.id
WHERE holder_obj.reachable = 1
  AND od.value_string LIKE '%<KEYWORD_OR_TAG>%'
GROUP BY holder_class_name, field_name, matched_string
ORDER BY reference_count DESC
LIMIT 25;
```

#### 3. Multi-Hop Traversal: Walk from Container/Holder to Contained String Values

Traverse 2 hops from a container or manager object through intermediate objects to their
`String` fields (e.g., `Manager -> collection -> Item -> String`):

```sql
SELECT
  COALESCE(parent_class.deobfuscated_name, parent_class.name) AS parent_class,
  COALESCE(r1.deobfuscated_field_name, r1.field_name) AS step1_field,
  COALESCE(child_class.deobfuscated_name, child_class.name) AS child_class,
  COALESCE(r2.deobfuscated_field_name, r2.field_name) AS step2_field,
  od.value_string AS string_value,
  COUNT(*) AS count
FROM heap_graph_object parent_obj
JOIN heap_graph_class parent_class ON parent_obj.type_id = parent_class.id
JOIN heap_graph_reference r1 ON parent_obj.id = r1.owner_id
JOIN heap_graph_object child_obj ON r1.owned_id = child_obj.id
JOIN heap_graph_class child_class ON child_obj.type_id = child_class.id
JOIN heap_graph_reference r2 ON child_obj.id = r2.owner_id
JOIN heap_graph_object str_obj ON r2.owned_id = str_obj.id
JOIN heap_graph_object_data od ON str_obj.object_data_id = od.id
WHERE parent_obj.reachable = 1
  AND COALESCE(parent_class.deobfuscated_name, parent_class.name) LIKE '%<HOLDER_CLASS>%'
  AND od.value_string IS NOT NULL
GROUP BY parent_class, step1_field, child_class, step2_field, string_value
ORDER BY count DESC
LIMIT 50;
```

#### 4. Query Primitive Fields (`boolean`, `int`, `long`, etc.) on a Class

```sql
SELECT
  COALESCE(c.deobfuscated_name, c.name) AS class_name,
  p.field_name,
  p.field_type,
  CASE p.field_type
    WHEN 'boolean' THEN CASE WHEN p.bool_value != 0 THEN 'true' ELSE 'false' END
    WHEN 'byte' THEN CAST(p.byte_value AS TEXT)
    WHEN 'char' THEN CAST(p.char_value AS TEXT)
    WHEN 'short' THEN CAST(p.short_value AS TEXT)
    WHEN 'int' THEN CAST(p.int_value AS TEXT)
    WHEN 'long' THEN CAST(p.long_value AS TEXT)
    WHEN 'float' THEN CAST(p.float_value AS TEXT)
    WHEN 'double' THEN CAST(p.double_value AS TEXT)
    ELSE ''
  END AS primitive_value,
  COUNT(*) AS instance_count
FROM heap_graph_object o
JOIN heap_graph_class c ON o.type_id = c.id
JOIN heap_graph_object_data od ON o.object_data_id = od.id
JOIN heap_graph_primitive p ON od.field_set_id = p.field_set_id
WHERE o.reachable = 1
  AND COALESCE(c.deobfuscated_name, c.name) LIKE '%<CLASS_NAME>%'
  AND p.field_name NOT LIKE 'java.lang.Object.shadow$%'
GROUP BY class_name, p.field_name, p.field_type, primitive_value
ORDER BY instance_count DESC
LIMIT 50;
```

#### 5. Inspect All Fields (Strings & Primitives) of a Specific Object Instance (`o.id = <OBJECT_ID>`)

Given an object ID found from dominator or reference traversal, fetch all its
`String` fields and primitive values:

```sql
-- String fields on specific object
SELECT
  COALESCE(r.deobfuscated_field_name, r.field_name) AS field_name,
  'java.lang.String' AS field_type,
  od.value_string AS field_value
FROM heap_graph_reference r
JOIN heap_graph_object str_obj ON r.owned_id = str_obj.id
JOIN heap_graph_object_data od ON str_obj.object_data_id = od.id
WHERE r.owner_id = <OBJECT_ID>
  AND od.value_string IS NOT NULL

UNION ALL

-- Primitive fields on specific object
SELECT
  p.field_name,
  p.field_type,
  CASE p.field_type
    WHEN 'boolean' THEN CASE WHEN p.bool_value != 0 THEN 'true' ELSE 'false' END
    WHEN 'byte' THEN CAST(p.byte_value AS TEXT)
    WHEN 'char' THEN CAST(p.char_value AS TEXT)
    WHEN 'short' THEN CAST(p.short_value AS TEXT)
    WHEN 'int' THEN CAST(p.int_value AS TEXT)
    WHEN 'long' THEN CAST(p.long_value AS TEXT)
    WHEN 'float' THEN CAST(p.float_value AS TEXT)
    WHEN 'double' THEN CAST(p.double_value AS TEXT)
    ELSE ''
  END AS field_value
FROM heap_graph_object o
JOIN heap_graph_object_data od ON o.object_data_id = od.id
JOIN heap_graph_primitive p ON od.field_set_id = p.field_set_id
WHERE o.id = <OBJECT_ID>;
```

## Reference: Technical Guardrails for Custom PerfettoSQL Queries

If you write custom PerfettoSQL queries beyond the scripts and recipes above,
you MUST follow these rules:

1.  **Use `android_heap_graph_class_aggregation` (Never Raw
    `SUM(dominated_size_bytes)`)**: If an instance of `ClassA` dominates another
    instance of `ClassA` (e.g. nested views or tree nodes), raw
    `SUM(dominated_size_bytes)` double-counts the child subtree. Always use
    `android_heap_graph_class_aggregation` (`INCLUDE PERFETTO MODULE
    android.memory.heap_graph.heap_graph_class_aggregation;`).
2.  **Always Scope by `process.name` (`upid`) and `graph_sample_ts`**: A trace
    may contain heap dumps for multiple processes or timestamps. Always join
    with `process pr ON upid = pr.id`, filter by `pr.name`, and select the
    target `graph_sample_ts`.
3.  **Escape Dollar Signs (`\$`) in Shell Queries or Use `-q`
    (`--query-file`)**: Java inner classes contain `$` (e.g.
    `ContentObserver$Transport`, `MainActivity$1`). Passing unescaped `$` inside
    double quotes (`trace_processor -Q "... 'Outer$Inner'"`) causes the shell to
    expand `$Inner` to an empty string. Always use `trace_processor -q
    query.sql` or escape `\$`.
4.  **Always Read Class Names via `COALESCE(deobfuscated_name, name)`**:
    `name` holds the raw name, which on an R8/ProGuard release build is a
    minified symbol (`a.b.c`). `deobfuscated_name` is populated only when the
    trace carries a deobfuscation mapping.
5.  **Compare Like With Like**: A class delta is only meaningful if both dumps
    come from the same process, the same build variant, and the same point in
    the user journey. Comparing a debug dump against a release dump, or a dump
    taken mid-animation against one taken at rest, produces deltas that have
    nothing to do with the change under investigation.

## Reference: Troubleshooting

-   **Class names look like `a.b.c` or `d.e$f`**: The dumps come from an
    R8/ProGuard-minified build and carry no deobfuscation mapping. Deltas and
    dominator paths are still correct and still worth reporting, but map the
    symbols back with the build's mapping file before naming a culprit class in
    the report; never guess what a minified symbol stands for.
-   **The report lists no regressed classes, or only decreases**: This is a
    valid result. Report it as such, together with the total reachable heap
    delta. Before concluding there is no regression, check the instance-count
    table too (a leak of many small objects moves counts but not bytes), and
    re-check that you compared the intended dumps — the report header prints
    which `graph_sample_ts` was selected out of how many available dumps.
-   **Total reachable heap shrank but a specific class grew**: Trust the
    per-class delta. Unrelated caches and `SoftReference` eviction move the
    process total by far more than a small leak does.
