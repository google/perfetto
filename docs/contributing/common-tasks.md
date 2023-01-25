# Common tasks

The checklists below show how to achieve some common tasks in the codebase.

## Add a new ftrace event

1. Find the `format` file for your event. The location of the file depends where `tracefs` is mounted but can often be found at `/sys/kernel/debug/tracing/events/EVENT_GROUP/EVENT_NAME/format`.
2. Copy the format file into the codebase at `src/traced/probes/ftrace/test/data/synthetic/events/EVENT_GROUP/EVENT_NAME/format`.
3. Add the event to [src/tools/ftrace_proto_gen/event_list](/src/tools/ftrace_proto_gen/event_list).
4. Run `tools/run_ftrace_proto_gen`. This will update `protos/perfetto/trace/ftrace/ftrace_event.proto` and `protos/perfetto/trace/ftrace/GROUP_NAME.proto`.
5. Run `tools/gen_all out/YOUR_BUILD_DIRECTORY`. This will update `src/traced/probes/ftrace/event_info.cc` and `protos/perfetto/trace/perfetto_trace.proto`.
6. If special handling in `trace_processor` is desired update [src/trace_processor/importers/ftrace/ftrace_parser.cc](/src/trace_processor/importers/ftrace/ftrace_parser.cc) to parse the event.
7. Upload and land your change as normal.

Here is an [example change](https://android-review.googlesource.com/c/platform/external/perfetto/+/1290645) which added the `ion/ion_stat` event.

## Contribute to SQL standard library

1. Add or edit an SQL file inside `perfetto/src/trace_processor/stdlib/`.
2. For a new file inside an existing module add the file to the corresponding `BUILD.gn`.
3. For a new module (subdirectory of `/stdlib/`), module name (directory name) has to be added to the list in `/stdlib/BUILD.gn`.

Files inside the standard library have to be formatted in a very specific way, as its structure is used to generate documentation. There are presubmit checks, but they are not infallible.

- Running the file cannot generate any data. There can be only `CREATE_FUNCTION`, `CREATE TABLE/VIEW` or `CREATE_VIEW_FUNCTION` inside.
- The name of each table/view/function needs to start with `{module_name}_` or `{internal_}`. Views/tables are must be `[a-z_]`, while functions are `[A-Z_]`. When a module is imported (using the `IMPORT` function), objects prefixed with internal should not be used.
  - The only exception is the `common` module. The name of functions/views/tables inside should not be prefixed with `common_`, as they are supposed to be module agnostic and widely used.
- Every non internal object has be prefixed with an SQL comment following a particular documentation schema e.g. similar to javadoc. The schema is a comment directly over the SQL which creates it, without empty lines. Any text is going to be parsed as markdown, so usage of markdown functionality (code, links, lists) is encouraged. Whitespaces in anything apart from descriptions are ignored, so comments can be formatted neatly. If the line with description exceeds 80 chars, description can be continued in following lines.
  - **Table/view**: each has to have object description and list of columns.
    - Description is any text above column comments.
    - For each column there has to be a comment line `-- @column {col name} {col description}`.
  - **Functions**: each has to have a function description, list of arguments (names, types, description) and description of return value in this order.
    - Function description is any text above argument comments.
    - For each argument there has to be a comment line `-- @arg {arg name} {arg type} {arg description}`. Arg name should follow `[a-z_]*`, arg type has to be exactly the same as specified in the function, so `[A-Z]*`.
    - Return comment is `-- @ret {return type} {return description}`. Return type should be exactly the same as specified in the function, so `[A-Z]*`.
  - **View functions**: each has to have a function description, list of arguments (names, types, description) and list of columns.
    - Function description is any text above argument comments.
    - For each argument there has to be a comment line `-- @arg {arg name} {arg type} {arg description}`. Arg name should follow `[a-z_]*`, arg type has to be exactly the same as specified in the function, so `[A-Z]*`.
    - For each column there has to be a comment line `-- @column {col name} {col description}`.

NOTE: Break lines outside of import description will be ignored.

Example of properly formatted view in module `android`:
```sql
-- Count Binder transactions per process.
--
-- @column process_name  Name of the process that started the binder transaction.
-- @column pid           PID of the process that started the binder transaction.
-- @column slice_name    Name of the slice with binder transaction.
-- @column event_count   Number of binder transactions in process in slice.
CREATE VIEW android_binder_metrics_by_process AS
SELECT
  process.name AS process_name,
  process.pid AS pid,
  slice.name AS slice_name,
  COUNT(*) AS event_count
FROM slice
INNER JOIN thread_track ON slice.track_id = thread_track.id
INNER JOIN thread ON thread.utid = thread_track.utid
INNER JOIN process ON thread.upid = process.upid
WHERE
  slice.name GLOB 'binder*'
GROUP BY
  process_name,
  slice_name;
```

Example of function in module `common`:
```sql
-- Extracts an int value with the given name from the metadata table.
--
-- @arg name STRING The name of the metadata entry.
-- @ret LONG int_value for the given name. NULL if there's no such entry.
SELECT CREATE_FUNCTION(
    'EXTRACT_INT_METADATA(name STRING)',
    'LONG',
    'SELECT int_value FROM metadata WHERE name = ($name)');
```

Example of view function in module `android`:
```sql
-- Given a launch id and GLOB for a slice name, returns columns for matching slices.
--
-- @arg launch_id INT         Id of launch.
-- @arg slice_name STRING     Name of slice with launch.
-- @column slice_name         Name of slice with launch.
-- @column slice_ts INT       Timestamp of slice start.
-- @column slice_dur INT      Duration of slice.
-- @column thread_name STRING Name of thread with slice
-- @column arg_set_id INT     Arg set id.
SELECT CREATE_VIEW_FUNCTION(
  'ANDROID_SLICES_FOR_LAUNCH_AND_SLICE_NAME(launch_id INT, slice_name STRING)',
  'slice_name STRING, slice_ts INT, slice_dur INT, thread_name STRING, arg_set_id INT',
  '
    SELECT slice_name, slice_ts, slice_dur, thread_name, arg_set_id
    FROM thread_slices_for_all_launches
    WHERE launch_id = $launch_id AND slice_name GLOB $slice_name
  '
);
```


## {#new-metric} Add a new trace-based metric

1. Create the proto file containing the metric in the [protos/perfetto/metrics](/protos/perfetto/metrics) folder. The appropriate` BUILD.gn` file should be updated as well.
2. Import the proto in [protos/perfetto/metrics/metrics.proto](/protos/perfetto/metrics/metrics.proto) and add a field for the new message.
3. Run `tools/gen_all out/YOUR_BUILD_DIRECTORY`. This will update the generated headers containing the descriptors for the proto.
  * *Note: this step has to be performed any time any metric-related proto is modified.*
  * If you don't see anything inside the `out/` directory you might have to
  rerun `tools/setup_all_configs.py`.
4. Add a new SQL file for the metric to [src/trace_processor/metrics](/src/trace_processor/metrics). The appropriate `BUILD.gn` file should be updated as well.
  * To learn how to write new metrics, see the [trace-based metrics documentation](/docs/analysis/metrics.md).
5. Build all targets in your out directory with `tools/ninja -C out/YOUR_BUILD_DIRECTORY`.
6. Add a new diff test for the metric. This can be done by adding files to
the `tests.*.py` files in a proper [test/trace_processor](/test/trace_processor) subfolder.
1. Run the newly added test with `tools/diff_test_trace_processor.py <path to trace processor binary>`.
2. Upload and land your change as normal.

Here is an [example change](https://android-review.googlesource.com/c/platform/external/perfetto/+/1290643) which added the `time_in_state` metric.

## Add a new trace processor table

1. Create the new table in the appropriate header file in [src/trace_processor/tables](/src/trace_processor/tables) by copying one of the existing macro definitions.
  * Make sure to understand whether a root or derived table is needed and copy the appropriate one. For more information see the [trace processor](/docs/analysis/trace-processor.md) documentation.
2. Register the table with the trace processor in the constructor for the [TraceProcessorImpl class](/src/trace_processor/trace_processor_impl.cc).
3. If also implementing ingestion of events into the table:
  1. Modify the appropriate parser class in [src/trace_processor/importers](/src/trace_processor/importers) and add the code to add rows to the newly added table.
  2. Add a new diff test for the added parsing code and table using
  `tools/add_tp_diff_test.py`.
  3. Run the newly added test with `tools/diff_test_trace_processor.py <path to trace processor binary>`.
4. Upload and land your change as normal.

## Adding new derived events

As derived events depend on metrics, the initial steps are same as that of developing a metric (see above).

NOTE: the metric can be just an empty proto message during prototyping or if no summarization is necessary. However, generally if an event is important enough to display in the UI, it should also be tracked in benchmarks as a metric.

To extend a metric with annotations:

1. Create a new table or view with the name `<metric name>_event`.
  * For example, for the [`android_startup`]() metric, we create a view named `android_startup_event`.
  * Note that the trailing `_event` suffix in the table name is important.
  * The schema required for this table is given below.
2. List your metric in the `initialiseHelperViews` method of `trace_controller.ts`.
3. Upload and land your change as normal.

The schema of the `<metric name>_event` table/view is as follows:

| Name         | Type     | Presence                              | Meaning                                                                                                                                                                                                                                     |
| :----------- | -------- | ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `track_type` | `string` | Mandatory                             | 'slice' for slices, 'counter' for counters                                                                                                                                                                                                  |
| `track_name` | `string` | Mandatory                             | Name of the track to display in the UI. Also the track identifier i.e. all events with same `track_name` appear on the same track.                                                                                                          |
| `ts`         | `int64`  | Mandatory                             | The timestamp of the event (slice or counter)                                                                                                                                                                                               |
| `dur`        | `int64`  | Mandatory for slice, NULL for counter | The duration of the slice                                                                                                                                                                                                                   |
| `slice_name` | `string` | Mandatory for slice, NULL for counter | The name of the slice                                                                                                                                                                                                                       |
| `value`      | `double` | Mandatory for counter, NULL for slice | The value of the counter                                                                                                                                                                                                                    |
| `group_name` | `string` | Optional                              | Name of the track group under which the track appears. All tracks with the same `group_name` are placed under the same group by that name. Tracks that lack this field or have NULL value in this field are displayed without any grouping. |

#### Known issues:

* Nested slices within the same track are not supported. We plan to support this
  once we have a concrete usecase.
* Tracks are always created in the global scope. We plan to extend this to
  threads and processes in the near future with additional contexts added as
  necessary.
* Instant events are currently not supported in the UI but this will be
  implemented in the near future. In trace processor, instants are always `0`
  duration slices with special rendering on the UI side.
* There is no way to tie newly added events back to the source events in the
  trace which were used to generate them. This is not currently a priority but
  something we may add in the future.
