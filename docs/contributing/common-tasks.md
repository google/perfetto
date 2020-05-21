# Common tasks

The checklists below show how to achieve some common tasks in the codebase.

## Add a new ftrace event

1. Find the `format` file for your event. The location of the file depends where `tracefs` is mounted but can often be found at `/sys/kernel/debug/tracing/events/EVENT_GROUP/EVENT_NAME/format`.
2. Copy the format file into the codebase at `src/traced/probes/ftrace/test/data/synthetic/events/EVENT_GROUP/EVENT_NAME/format`.
3. Add the event to [tools/ftrace_proto_gen/event_whitelist](/tools/ftrace_proto_gen/event_whitelist).
4. Run `tools/run_ftrace_proto_gen`. This will update `protos/perfetto/trace/ftrace/ftrace_event.proto` and `protos/perfetto/trace/ftrace/GROUP_NAME.proto`.
5. Run `tools/gen_all out/YOUR_BUILD_DIRECTORY`. This will update `src/traced/probes/ftrace/event_info.cc` and `protos/perfetto/trace/perfetto_trace.proto`.
6. If special handling in `trace_processor` is desired update [src/trace_processor/importers/ftrace/ftrace_parser.cc](/src/trace_processor/importers/ftrace/ftrace_parser.cc) to parse the event.
7. Upload and land your change as normal.

Here is an [example change](https://android-review.googlesource.com/c/platform/external/perfetto/+/1290645) which added the `ion/ion_stat` event.

## {#new-metric} Add a new trace-based metric

1. Create the proto file containing the metric in the [protos/perfetto/metrics](/protos/perfetto/metrics) folder. The appropriate` BUILD.gn` file should be updated as well.
2. Import the proto in [protos/perfetto/metrics/metrics.proto](/protos/perfetto/metrics/metrics.proto) and add a field for the new message.
3. Run `tools/gen_all out/YOUR_BUILD_DIRECTORY`. This will update the generated headers containing the descriptors for the proto.
  * *Note: this step has to be performed any time any metric-related proto is modified.*
4. Add a new SQL file for the metric to [src/trace_processor/metrics](/src/trace_processor/metrics). The appropriate `BUILD.gn` file should be updated as well.
  * To learn how to write new metrics, see the [trace-based metrics documentation](/docs/analysis/metrics.md).
5. Build all targets in your out directory with `tools/ninja -C out/YOUR_BUILD_DIRECTORY`.
6. Add a new diff test for the metric. This can be done by adding files to the [test/metrics](/test/metrics) folder and modifying the [index file](/test/metrics/index).
7. Run the newly added test with `tools/diff_test_trace_processor.py <path to trace processor binary>`.
8. Upload and land your change as normal.

Here is an [example change](https://android-review.googlesource.com/c/platform/external/perfetto/+/1290643) which added the `time_in_state` metric.

## Add a new trace processor table

1. Create the new table in the appropriate header file in [src/trace_processor/tables](/src/trace_processor/tables) by copying one of the existing macro definitions.
  * Make sure to understand whether a root or derived table is needed and copy the appropriate one. For more information see the [trace processor](/docs/analysis/trace-processor.md) documentation.
2. Register the table with the trace processor in the constructor for the [TraceProcessorImpl class](/src/trace_processor/trace_processor_impl.cc).
3. If also implementing ingestion of events into the table:
  1. Modify the appropriate parser class in [src/trace_processor/importers](/src/trace_processor/importers) and add the code to add rows to the newly added table.
  2. Add a new diff test for the added parsing code and table using `tools/add_tp_diff_test.sh`.
    * Make sure to modify the [index file](/test/trace_processor/index) to correctly organize the test with other similar tests.
  3. Run the newly added test with `tools/diff_test_trace_processor.py <path to trace processor binary>`.
4. Upload and land your change as normal.

## {#new-annotation} Add a new annotation

NOTE: all currently implemented annotations are based only on the name of the slice. It is straightforward to extend this to also consider ancestors and other similar properties; we plan on doing this in the future.

1. Change the [`DescribeSlice`](/src/trace_processor/analysis/describe_slice.h) function as appropriate.
  * The inputs are the table containing all the slices from the trace and the id of the slice which an embedder (e.g. the UI) is requesting a description for.
  * The output is a `SliceDescription` which is simply a `pair<description, doc link>`.
2. Upload and land your change as normal.

## Adding new derived events

As derived events depend on metrics, the initial steps are same as that of developing a metric (see above).

NOTE: the metric can be just an empty proto message during prototyping or if no summarization is necessary. However, generally if an event is important enough to display in the UI, it should also be tracked in benchmarks as a metric.

To extend a metric with annotations:

1. Create a new table or view with the name `<metric name>_annotations`.
  * For example, for the [`android_startup`]() metric, we create a view named `android_startup_annotations`.
  * Note that the trailing `_annotations` suffix in the table name is important.
  * The schema required for this table is given below.

2. Upload and land your change as normal.

The schema of the `<metric name>_annotations` table/view is as follows:

| Name         | Type     | Presence                              | Meaning                                                      |
| :----------- | -------- | ------------------------------------- | ------------------------------------------------------------ |
| `track_type` | `string` | Mandatory                             | 'slice' for slices, 'counter' for counters                   |
| `track_name` | `string` | Mandatory                             | Name of the track to display in the UI. Also the track identifier i.e. all events with same `track_name` appear on the same track. |
| `ts`         | `int64`  | Mandatory                             | The timestamp of the event (slice or counter)                |
| `dur`        | `int64`  | Mandatory for slice, NULL for counter | The duration of the slice                                    |
| `slice_name` | `string` | Mandatory for slice, NULL for counter | The name of the slice                                        |
| `value`      | `double` | Mandatory for counter, NULL for slice | The value of the counter                                     |

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
