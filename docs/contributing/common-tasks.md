# Common tasks

Most contributions to Perfetto fall into one of the categories below.

## UI

Most UI contributions should involve creating or modifying plugins.
See the [UI plugins page](ui-plugins) for instructions.

## Trace Processor

### Contribute to SQL standard library

1. Add or edit an SQL file inside `perfetto/src/trace_processor/perfetto_sql/stdlib/`. This SQL file will be a new standard library module.
2. For a new file inside an existing package add the file to the corresponding `BUILD.gn`.
3. For a new package (subdirectory of `/stdlib/`), the package name (directory name) has to be added to the list in `/stdlib/BUILD.gn`.

Standard library files must follow a specific format because their structure is
used to generate documentation. Presubmit checks catch some formatting errors,
but they are not infallible.

- Running the file cannot generate any data. There can be only `CREATE PERFETTO {FUNCTION|TABLE|VIEW|MACRO}` statements inside.
- The name of each standard library object needs to start with `{module_name}_` or be prefixed with an underscore(`_`) for internal objects.
  The names must only contain lower and upper case letters and underscores. When a module is included (using the `INCLUDE PERFETTO MODULE`) the internal objects should not be treated as an API.
- Every table or view should have [a schema](/docs/analysis/perfetto-sql-syntax.md#schema).

#### Documentation

- Every non-internal object, its function arguments, and its schema columns must
  have a preceding SQL comment documenting them.
- Text is parsed as Markdown, so use formatting such as code, links, and lists.
  Whitespace outside descriptions is ignored, so you can format comments neatly.
  If a description exceeds 80 characters, continue it on the following lines.
  - **Table/view**: each must have a schema, an object description, and a
    comment above each column's definition in the schema.
    - The description is the text in the comment above the
      `CREATE PERFETTO {TABLE,VIEW}` statement.
    - A column's comment is the text immediately above its definition in the
      schema.
  - **Scalar Functions**: each must have a function description followed by a
    return value description.
    - The function description is the text in the comment above the
      `CREATE PERFETTO FUNCTION` statement.
    - Each argument must have a comment line immediately above its definition.
    - The return comment should immediately precede `RETURNS`.
  - **Table Functions**: each must have a function description, a list of
    arguments (names, types, descriptions), and a list of columns.
    - The function description is the text in the comment above the
      `CREATE PERFETTO FUNCTION` statement.
    - Each argument must have a comment line immediately above its definition.
    - Each column must have a comment line immediately above its definition.

NOTE: Line breaks outside the import description are ignored.

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

Generally, you do not have to worry about version skew between the UI and
`trace_processor` because they are built together at the same commit. However,
version skew can occur when running `trace_processor` in HTTP RPC mode
(`trace_processor server http`), which lets you use a native `trace_processor`
instance with the UI.

A common case is when the UI is more recent than `trace_processor` and depends
on a new table definition. With older versions of `trace_processor` in HTTP RPC
mode, the UI crashes when it tries to query a nonexistent table. To avoid this,
we use a version number. If `trace_processor` reports a version older than the
one the UI was built with, we prompt the user to update.

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

This
[example change](https://android-review.googlesource.com/c/platform/external/perfetto/+/3343525)
added a new event. Note: Perfetto's source of truth has since moved to GitHub.
The change's content is still accurate, but you should send patches via GitHub,
not AOSP Gerrit.

To test your changes, you can sideload your locally built `tracebox` binary on an Android device. See [Sideloading on Android](#sideloading) for more details.

## {#sideloading} Sideloading on Android

To test changes on an Android device (e.g. when adding a new ftrace event),
you can use the `record_android_trace` script to sideload a locally built
`tracebox` binary.

1. Build `tracebox` for Android.
   See [Getting Started](getting-started.md#building) for instructions
   on how to configure the build for Android.
   ```bash
   # This assumes you have configured out/android per the instructions linked above.
   tools/ninja -C out/android_release_arm64 tracebox
   ```

2. Use `record_android_trace` to sideload and record a trace:
   ```bash
   tools/record_android_trace \
     --sideload-path out/android_release_arm64/tracebox \
     -o trace.pftrace \
     -t 10s \
     -b 32mb \
     sched/sched_switch
   ```
   The `--sideload-path` argument tells the script to push the locally built
   `tracebox` binary to the device and use it for recording.

## Statsd

### Update statsd descriptor

Perfetto has limited support for statsd atoms it does not know about:

- You must refer to them using `raw_atom_id` in the config.
- They appear as `atom_xxx.field_yyy` in trace processor.
- Only top-level messages are parsed.

To update Perfetto's descriptor and handle new atoms from AOSP without these
limitations:

1. Run `tools/update-statsd-descriptor`.
2. Upload and land your change as normal.
