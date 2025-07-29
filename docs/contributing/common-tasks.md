# Common tasks

Most contributions to Perfetto falls into one of below categories.

## UI

With the pluginization of the UI most of the contributions to the UI should be related to creating/modifying plugins.
Go to [UI plugins page](ui-plugins) to learn how to do it.

## Trace Processor

### Contribute to SQL standard library

1. Add or edit an SQL file inside `perfetto/src/trace_processor/stdlib/`. This SQL file will be a new standard library module.
2. For a new file inside an existing package add the file to the corresponding `BUILD.gn`.
3. For a new package (subdirectory of `/stdlib/`), the package name (directory name) has to be added to the list in `/stdlib/BUILD.gn`.

Files inside the standard library have to be formatted in a very specific way, as its structure is used to generate documentation. There are presubmit checks, but they are not infallible.

- Running the file cannot generate any data. There can be only `CREATE PERFETTO {FUNCTION|TABLE|VIEW|MACRO}` statements inside.
- The name of each standard library object needs to start with `{module_name}_` or be prefixed with an underscore(`_`) for internal objects.
  The names must only contain lower and upper case letters and underscores. When a module is included (using the `INCLUDE PERFETTO MODULE`) the internal objects should not be treated as an API.
- Every table or view should have [a schema](/docs/analysis/perfetto-sql-syntax.md#tableview-schema).

#### Documentation

- Every non internal object, as well as its function arguments and columns in its schema have to be prefixed with an SQL comment documenting it.
- Any text is going to be parsed as markdown, so usage of markdown functionality (code, links, lists) is encouraged.
  Whitespaces in anything apart from descriptions are ignored, so comments can be formatted neatly.
  If the line with description exceeds 80 chars, description can be continued in following lines.
  - **Table/view**: each has to have schema, object description and a comment above each column's definition in the schema.
    - Description is any text in the comment above `CREATE PERFETTO {TABLE,VIEW}` statement.
    - Column's comment is the text immediately above column definition in the schema.
  - **Scalar Functions**: each has to have a function description and description of return value in this order.
    - Function description is any text in the comment above `CREATE PERFETTO FUNCTION` statement.
    - For each argument there has to be a comment line immediately above argument definition.
    - Return comment should immediately precede `RETURNS`.
  - **Table Functions**: each has to have a function description, list of arguments (names, types, description) and list of columns.
    - Function description is any text in the comment above `CREATE PERFETTO FUNCTION` statement.
    - For each argument there has to be a comment line immediately above argument definition.
    - For each column there has to be a comment line immediately above column definition.

NOTE: Break lines outside of import description will be ignored.

Example of properly formatted view in module `android`:

```sql
-- Count Binder transactions per process.
CREATE PERFETTO VIEW android_binder_metrics_by_process(
  -- Name of the process that started the binder transaction.
  process_name STRING,
  -- PID of the process that started the binder transaction.
  pid INT,
  -- Name of the slice with binder transaction.
  slice_name STRING,
  -- Number of binder transactions in process in slice.
  event_count INT
) AS
SELECT
  process.name AS process_name,
  process.pid AS pid,
  slice.name AS slice_name,
  COUNT(*) AS event_count
FROM slice
JOIN thread_track ON slice.track_id = thread_track.id
JOIN thread ON thread.utid = thread_track.utid
JOIN process ON thread.upid = process.upid
WHERE
  slice.name GLOB 'binder*'
GROUP BY
  process_name,
  slice_name;
```

Example of table function in module `android`:

```sql
-- Given a launch id and GLOB for a slice name, returns columns for matching slices.
CREATE PERFETTO FUNCTION ANDROID_SLICES_FOR_LAUNCH_AND_SLICE_NAME(
  -- Id of launch.
  launch_id INT,
  -- Name of slice with launch.
  slice_name STRING
)
RETURNS TABLE(
  -- Name of slice with launch.
  slice_name STRING,
  -- Timestamp of slice start.
  slice_ts TIMESTAMP,
  -- Duration of slice.
  slice_dur DURATION,
  -- Name of thread with slice.
  thread_name STRING,
  -- Arg set id.
  arg_set_id ARGSETID
)
AS
SELECT
  slice_name,
  slice_ts,
  slice_dur,
  thread_name,
  arg_set_id
FROM thread_slices_for_all_launches
WHERE launch_id = $launch_id AND slice_name GLOB $slice_name;
```

### Add a new trace processor table

1. Create the new table in the appropriate header file in [src/trace_processor/tables](/src/trace_processor/tables) by copying one of the existing macro definitions.

- Make sure to understand whether a root or derived table is needed and copy the appropriate one. For more information see the [trace processor](/docs/analysis/trace-processor.md) documentation.

2. Register the table with the trace processor in the constructor for the [TraceProcessorImpl class](/src/trace_processor/trace_processor_impl.cc).
3. If also implementing ingestion of events into the table:
   1. Modify the appropriate parser class in [src/trace_processor/importers](/src/trace_processor/importers) and add the code to add rows to the newly added table.
   2. Add a new diff test for the added parsing code and table.
   3. Run the newly added test with `tools/diff_test_trace_processor.py <path to trace processor shell binary>`.
4. Upload and land your change as normal.

### Update `TRACE_PROCESSOR_CURRENT_API_VERSION`

Generally you do not have to worry about version skew between the UI
and the `trace_processor` since they are built together at the same
commit. However version skew can occur when using the `--httpd` mode
which allows a native `trace_processor` instance to be used with the UI.

A common case is when the UI is more recent than `trace_processor`
and depends on a new table definition. With older versions of
`trace_processor` in `--httpd` mode the UI crashes attempting to query
a non-existant table. To avoid this we use a version number. If the
version number `trace_processor` reports is older than the one the UI
was built with we prompt the user to update.

1. Go to `protos/perfetto/trace_processor/trace_processor.proto`
2. Increment `TRACE_PROCESSOR_CURRENT_API_VERSION`
3. Add a comment explaining what has changed.

### {#new-metric} Add a new trace-based metric

1. Create the proto file containing the metric in the [protos/perfetto/metrics](/protos/perfetto/metrics) folder. The appropriate` BUILD.gn` file should be updated as well.
2. Import the proto in [protos/perfetto/metrics/metrics.proto](/protos/perfetto/metrics/metrics.proto) and add a field for the new message.
3. Run `tools/gen_all out/YOUR_BUILD_DIRECTORY`. This will update the generated headers containing the descriptors for the proto.

- _Note: this step has to be performed any time any metric-related proto is modified._
- If you don't see anything inside the `out/` directory you might have to
  rerun `tools/setup_all_configs.py`.

4. Add a new SQL file for the metric to [src/trace_processor/metrics](/src/trace_processor/metrics). The appropriate `BUILD.gn` file should be updated as well.

- To learn how to write new metrics, see the [trace-based metrics documentation](/docs/analysis/metrics.md).

5. Build all targets in your out directory with `tools/ninja -C out/YOUR_BUILD_DIRECTORY`.
6. Add a new diff test for the metric. This can be done by adding files to
   the `tests_*.py` files in a proper [test/trace_processor](/test/trace_processor) subfolder.
7. Run the newly added test with `tools/diff_test_trace_processor.py <path to trace processor binary>`.
8. Upload and land your change as normal.

## Ftrace

### Add a new ftrace event

1. Find the `format` file for your event. The location of the file depends where `tracefs` is mounted but can often be found at `/sys/kernel/debug/tracing/events/EVENT_GROUP/EVENT_NAME/format`.
2. Copy the format file into the codebase at `src/traced/probes/ftrace/test/data/synthetic/events/EVENT_GROUP/EVENT_NAME/format`.
3. Add the event to [src/tools/ftrace_proto_gen/event_list](/src/tools/ftrace_proto_gen/event_list).
4. Run `tools/run_ftrace_proto_gen`. This will update `protos/perfetto/trace/ftrace/ftrace_event.proto` and `protos/perfetto/trace/ftrace/GROUP_NAME.proto`.
5. Run `tools/gen_all out/YOUR_BUILD_DIRECTORY`. This will update `src/traced/probes/ftrace/event_info.cc` and `protos/perfetto/trace/perfetto_trace.proto`.
6. If special handling in `trace_processor` is desired update [src/trace_processor/importers/ftrace/ftrace_parser.cc](/src/trace_processor/importers/ftrace/ftrace_parser.cc) to parse the event.
7. Upload and land your change as normal.

Here is an [example change](https://android-review.googlesource.com/c/platform/external/perfetto/+/1290645) which added the `ion/ion_stat` event.

## Statsd

### Update statsd descriptor

Perfetto has limited support for statsd atoms it does not know about.

- Must be referred to using `raw_atom_id` in the config.
- Show up as `atom_xxx.field_yyy` in trace processor.
- Only top level messages are parsed.

To update Perfetto's descriptor and handle new atoms from AOSP without these
limitations:

1. Run `tools/update-statsd-descriptor`.
2. Upload and land your change as normal.
