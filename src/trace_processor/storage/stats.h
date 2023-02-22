/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_STORAGE_STATS_H_
#define SRC_TRACE_PROCESSOR_STORAGE_STATS_H_

#include <stddef.h>

namespace perfetto {
namespace trace_processor {
namespace stats {

// Compile time list of parsing and processing stats.
// clang-format off
#define PERFETTO_TP_STATS(F)                                                   \
  F(android_br_parse_errors,              kSingle,  kError,    kTrace,    ""), \
  F(android_log_num_failed,               kSingle,  kError,    kTrace,    ""), \
  F(android_log_format_invalid,           kSingle,  kError,    kTrace,    ""), \
  F(android_log_num_skipped,              kSingle,  kInfo,     kTrace,    ""), \
  F(android_log_num_total,                kSingle,  kInfo,     kTrace,    ""), \
  F(counter_events_out_of_order,          kSingle,  kError,    kAnalysis, ""), \
  F(deobfuscate_location_parse_error,     kSingle,  kError,    kTrace,    ""), \
  F(energy_breakdown_missing_values,      kSingle,  kError,    kAnalysis, ""), \
  F(energy_descriptor_invalid,            kSingle,  kError,    kAnalysis, ""), \
  F(entity_state_descriptor_invalid,      kSingle,  kError,    kAnalysis, ""), \
  F(entity_state_residency_invalid,       kSingle,  kError,    kAnalysis, ""), \
  F(entity_state_residency_lookup_failed, kSingle,  kError,    kAnalysis, ""), \
  F(energy_uid_breakdown_missing_values,  kSingle,  kError,    kAnalysis, ""), \
  F(frame_timeline_event_parser_errors,   kSingle,  kInfo,     kAnalysis, ""), \
  F(ftrace_bundle_tokenizer_errors,       kSingle,  kError,    kAnalysis, ""), \
  F(ftrace_cpu_bytes_read_begin,          kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_bytes_read_end,            kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_bytes_read_delta,          kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_commit_overrun_begin,      kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_commit_overrun_end,        kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_commit_overrun_delta,      kIndexed, kError,    kTrace,    ""), \
  F(ftrace_cpu_dropped_events_begin,      kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_dropped_events_end,        kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_dropped_events_delta,      kIndexed, kError,    kTrace,    ""), \
  F(ftrace_cpu_entries_begin,             kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_entries_end,               kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_entries_delta,             kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_now_ts_begin,              kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_now_ts_end,                kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_oldest_event_ts_begin,     kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_oldest_event_ts_end,       kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_overrun_begin,             kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_overrun_end,               kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_overrun_delta,             kIndexed, kDataLoss, kTrace,         \
      "The kernel ftrace buffer cannot keep up with the rate of events "       \
      "produced. Indexed by CPU. This is likely a misconfiguration."),         \
  F(ftrace_cpu_read_events_begin,         kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_read_events_end,           kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_cpu_read_events_delta,         kIndexed, kInfo,     kTrace,    ""), \
  F(ftrace_setup_errors,                  kSingle,  kError,    kTrace,         \
  "One or more atrace/ftrace categories were not found or failed to enable. "  \
  "See ftrace_setup_errors in the metadata table for more details."),          \
  F(fuchsia_non_numeric_counters,         kSingle,  kError,    kAnalysis, ""), \
  F(fuchsia_timestamp_overflow,           kSingle,  kError,    kAnalysis, ""), \
  F(fuchsia_invalid_event,                kSingle,  kError,    kAnalysis, ""), \
  F(gpu_counters_invalid_spec,            kSingle,  kError,    kAnalysis, ""), \
  F(gpu_counters_missing_spec,            kSingle,  kError,    kAnalysis, ""), \
  F(gpu_render_stage_parser_errors,       kSingle,  kError,    kAnalysis, ""), \
  F(graphics_frame_event_parser_errors,   kSingle,  kInfo,     kAnalysis, ""), \
  F(guess_trace_type_duration_ns,         kSingle,  kInfo,     kAnalysis, ""), \
  F(interned_data_tokenizer_errors,       kSingle,  kInfo,     kAnalysis, ""), \
  F(invalid_clock_snapshots,              kSingle,  kError,    kAnalysis, ""), \
  F(invalid_cpu_times,                    kSingle,  kError,    kAnalysis, ""), \
  F(meminfo_unknown_keys,                 kSingle,  kError,    kAnalysis, ""), \
  F(mismatched_sched_switch_tids,         kSingle,  kError,    kAnalysis, ""), \
  F(mm_unknown_type,                      kSingle,  kError,    kAnalysis, ""), \
  F(parse_trace_duration_ns,              kSingle,  kInfo,     kAnalysis, ""), \
  F(power_rail_unknown_index,             kSingle,  kError,    kTrace,    ""), \
  F(proc_stat_unknown_counters,           kSingle,  kError,    kAnalysis, ""), \
  F(rss_stat_unknown_keys,                kSingle,  kError,    kAnalysis, ""), \
  F(rss_stat_negative_size,               kSingle,  kInfo,     kAnalysis, ""), \
  F(rss_stat_unknown_thread_for_mm_id,    kSingle,  kInfo,     kAnalysis, ""), \
  F(sched_switch_out_of_order,            kSingle,  kError,    kAnalysis, ""), \
  F(slice_out_of_order,                   kSingle,  kError,    kAnalysis, ""), \
  F(flow_duplicate_id,                    kSingle,  kError,    kTrace,    ""), \
  F(flow_no_enclosing_slice,              kSingle,  kError,    kTrace,    ""), \
  F(flow_step_without_start,              kSingle,  kInfo,     kTrace,    ""), \
  F(flow_end_without_start,               kSingle,  kInfo,     kTrace,    ""), \
  F(flow_invalid_id,                      kSingle,  kError,    kTrace,    ""), \
  F(flow_without_direction,               kSingle,  kError,    kTrace,    ""), \
  F(stackprofile_invalid_string_id,       kSingle,  kError,    kTrace,    ""), \
  F(stackprofile_invalid_mapping_id,      kSingle,  kError,    kTrace,    ""), \
  F(stackprofile_invalid_frame_id,        kSingle,  kError,    kTrace,    ""), \
  F(stackprofile_invalid_callstack_id,    kSingle,  kError,    kTrace,    ""), \
  F(stackprofile_parser_error,            kSingle,  kError,    kTrace,    ""), \
  F(systrace_parse_failure,               kSingle,  kError,    kAnalysis, ""), \
  F(task_state_invalid,                   kSingle,  kError,    kAnalysis, ""), \
  F(traced_buf_abi_violations,            kIndexed, kDataLoss, kTrace,    ""), \
  F(traced_buf_buffer_size,               kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_bytes_overwritten,         kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_bytes_read,                kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_bytes_written,             kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_chunks_discarded,          kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_chunks_overwritten,        kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_chunks_read,               kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_chunks_rewritten,          kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_chunks_written,            kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_chunks_committed_out_of_order,                                  \
                                          kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_padding_bytes_cleared,     kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_padding_bytes_written,     kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_patches_failed,            kIndexed, kDataLoss, kTrace,    ""), \
  F(traced_buf_patches_succeeded,         kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_readaheads_failed,         kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_readaheads_succeeded,      kIndexed, kInfo,     kTrace,    ""), \
  F(traced_buf_trace_writer_packet_loss,  kIndexed, kDataLoss, kTrace,    ""), \
  F(traced_buf_write_wrap_count,          kIndexed, kInfo,     kTrace,    ""), \
  F(traced_chunks_discarded,              kSingle,  kInfo,     kTrace,    ""), \
  F(traced_data_sources_registered,       kSingle,  kInfo,     kTrace,    ""), \
  F(traced_data_sources_seen,             kSingle,  kInfo,     kTrace,    ""), \
  F(traced_final_flush_failed,            kSingle,  kDataLoss, kTrace,    ""), \
  F(traced_final_flush_succeeded,         kSingle,  kInfo,     kTrace,    ""), \
  F(traced_flushes_failed,                kSingle,  kDataLoss, kTrace,    ""), \
  F(traced_flushes_requested,             kSingle,  kInfo,     kTrace,    ""), \
  F(traced_flushes_succeeded,             kSingle,  kInfo,     kTrace,    ""), \
  F(traced_patches_discarded,             kSingle,  kInfo,     kTrace,    ""), \
  F(traced_producers_connected,           kSingle,  kInfo,     kTrace,    ""), \
  F(traced_producers_seen,                kSingle,  kInfo,     kTrace,    ""), \
  F(traced_total_buffers,                 kSingle,  kInfo,     kTrace,    ""), \
  F(traced_tracing_sessions,              kSingle,  kInfo,     kTrace,    ""), \
  F(track_event_parser_errors,            kSingle,  kInfo,     kAnalysis, ""), \
  F(track_event_dropped_packets_outside_of_range_of_interest,                  \
                                          kSingle,  kInfo,     kAnalysis,      \
      "The number of TrackEvent packets dropped by trace processor due to "    \
      "being outside of the range of interest. This happens if a trace has a " \
      "TrackEventRangeOfInterest packet, and track event dropping is "         \
      "enabled."),                                                             \
  F(track_event_tokenizer_errors,         kSingle,  kInfo,     kAnalysis, ""), \
  F(track_event_thread_invalid_end,       kSingle,  kError,    kTrace,         \
      "The end event for a thread track does not match a track event "         \
      "begin event. This can happen on mixed atrace/track_event traces "       \
      "and is usually caused by data loss or bugs when the events are "        \
      "emitted. The outcome of this is that slices can appear to be closed "   \
      "before they were closed in reality"),                                   \
  F(tokenizer_skipped_packets,            kSingle,  kInfo,     kAnalysis, ""), \
  F(vmstat_unknown_keys,                  kSingle,  kError,    kAnalysis, ""), \
  F(vulkan_allocations_invalid_string_id,                                      \
                                          kSingle,  kError,    kTrace,    ""), \
  F(clock_sync_failure,                   kSingle,  kError,    kAnalysis, ""), \
  F(clock_sync_cache_miss,                kSingle,  kInfo,     kAnalysis, ""), \
  F(process_tracker_errors,               kSingle,  kError,    kAnalysis, ""), \
  F(json_tokenizer_failure,               kSingle,  kError,    kTrace,    ""), \
  F(json_parser_failure,                  kSingle,  kError,    kTrace,    ""), \
  F(json_display_time_unit,               kSingle,  kInfo,     kTrace,         \
      "The displayTimeUnit key was set in the JSON trace. In some prior "      \
      "versions of trace processor this key could effect how the trace "       \
      "processor parsed timestamps and durations. In this version the key is " \
      "ignored which more closely matches the bavahiour of catapult."),        \
  F(heap_graph_invalid_string_id,         kIndexed, kError,    kTrace,    ""), \
  F(heap_graph_non_finalized_graph,       kSingle,  kError,    kTrace,    ""), \
  F(heap_graph_malformed_packet,          kIndexed, kError,    kTrace,    ""), \
  F(heap_graph_missing_packet,            kIndexed, kError,    kTrace,    ""), \
  F(heapprofd_buffer_corrupted,           kIndexed, kError,    kTrace,         \
      "Shared memory buffer corrupted. This is a bug or memory corruption "    \
      "in the target. Indexed by target upid."),                               \
  F(heapprofd_hit_guardrail,              kIndexed, kError,    kTrace,         \
      "HeapprofdConfig specified a CPU or Memory Guardrail that was hit. "     \
      "Indexed by target upid."),                                              \
  F(heapprofd_buffer_overran,             kIndexed, kDataLoss, kTrace,         \
      "The shared memory buffer between the target and heapprofd overran. "    \
      "The profile was truncated early. Indexed by target upid."),             \
  F(heapprofd_client_error,               kIndexed, kError,    kTrace,         \
      "The heapprofd client ran into a problem and disconnected. "             \
      "See profile_packet.proto  for error codes."),                           \
  F(heapprofd_client_disconnected,        kIndexed, kInfo,     kTrace,    ""), \
  F(heapprofd_malformed_packet,           kIndexed, kError,    kTrace,    ""), \
  F(heapprofd_missing_packet,             kSingle,  kError,    kTrace,    ""), \
  F(heapprofd_rejected_concurrent,        kIndexed, kError,    kTrace,         \
      "The target was already profiled by another tracing session, so the "    \
      "profile was not taken. Indexed by target upid."),                       \
  F(heapprofd_non_finalized_profile,      kSingle,  kError,    kTrace,    ""), \
  F(heapprofd_sampling_interval_adjusted,                                      \
    kIndexed, kInfo,    kTrace,                                                \
      "By how many byes the interval for PID was increased "                   \
      "by adaptive sampling."),                                                \
  F(heapprofd_unwind_time_us,             kIndexed, kInfo,     kTrace,         \
      "Time spent unwinding callstacks."),                                     \
  F(heapprofd_unwind_samples,             kIndexed, kInfo,     kTrace,         \
      "Number of samples unwound."),                                           \
  F(heapprofd_client_spinlock_blocked,    kIndexed, kInfo,     kTrace,         \
       "Time (us) the heapprofd client was blocked on the spinlock."),         \
  F(heapprofd_last_profile_timestamp,     kIndexed, kInfo,     kTrace,         \
       "The timestamp (in trace time) for the last dump for a process"),       \
  F(symbolization_tmp_build_id_not_found,     kSingle,  kError,    kAnalysis,  \
       "Number of file mappings in /data/local/tmp without a build id. "       \
       "Symbolization doesn't work for executables in /data/local/tmp "        \
       "because of SELinux. Please use /data/local/tests"),                    \
  F(metatrace_overruns,                   kSingle,  kError,    kTrace,    ""), \
  F(packages_list_has_parse_errors,       kSingle,  kError,    kTrace,    ""), \
  F(packages_list_has_read_errors,        kSingle,  kError,    kTrace,    ""), \
  F(game_intervention_has_parse_errors,   kSingle,  kError,    kTrace,         \
       "One or more parsing errors occurred. This could result from "          \
       "unknown game more or intervention added to the file to be parsed."),   \
  F(game_intervention_has_read_errors,    kSingle,  kError,    kTrace,         \
       "The file to be parsed can't be opened. This can happend when "         \
       "the file name is not found or no permission to access the file"),      \
  F(compact_sched_has_parse_errors,       kSingle,  kError,    kTrace,    ""), \
  F(misplaced_end_event,                  kSingle,  kDataLoss, kAnalysis, ""), \
  F(truncated_sys_write_duration,         kSingle,  kDataLoss,  kAnalysis,     \
      "Count of sys_write slices that have a truncated duration to resolve "   \
      "nesting incompatibilities with atrace slices. Real durations "          \
      "can be recovered via the |raw| table."),                                \
  F(sched_waking_out_of_order,            kSingle,  kError,    kAnalysis, ""), \
  F(compact_sched_switch_skipped,         kSingle,  kInfo,     kAnalysis, ""), \
  F(compact_sched_waking_skipped,         kSingle,  kInfo,     kAnalysis, ""), \
  F(empty_chrome_metadata,                kSingle,  kError,    kTrace,    ""), \
  F(ninja_parse_errors,                   kSingle,  kError,    kTrace,    ""), \
  F(perf_cpu_lost_records,                kIndexed, kDataLoss, kTrace,    ""), \
  F(perf_process_shard_count,             kIndexed, kInfo,     kTrace,    ""), \
  F(perf_chosen_process_shard,            kIndexed, kInfo,     kTrace,    ""), \
  F(perf_guardrail_stop_ts,               kIndexed, kDataLoss, kTrace,    ""), \
  F(perf_samples_skipped,                 kSingle,  kInfo,     kTrace,    ""), \
  F(perf_samples_skipped_dataloss,        kSingle,  kDataLoss, kTrace,    ""), \
  F(memory_snapshot_parser_failure,       kSingle,  kError,    kAnalysis, ""), \
  F(thread_time_in_state_out_of_order,    kSingle,  kError,    kAnalysis, ""), \
  F(thread_time_in_state_unknown_cpu_freq,                                     \
                                          kSingle,  kError,    kAnalysis, ""), \
  F(ftrace_packet_before_tracing_start,   kSingle,  kInfo,     kAnalysis,      \
      "An ftrace packet was seen before the tracing start timestamp from "     \
      "the tracing service. This happens if the ftrace buffers were not "      \
      "cleared properly. These packets are silently dropped by trace "         \
      "processor."),                                                           \
  F(sorter_push_event_out_of_order,       kSingle, kError,     kTrace,         \
      "Trace events are out of order event after sorting. This can happen "    \
      "due to many factors including clock sync drift, producers emitting "    \
      "events out of order or a bug in trace processor's logic of sorting."),  \
  F(unknown_extension_fields,             kSingle,  kError,    kTrace,         \
      "TraceEvent had unknown extension fields, which might result in "        \
      "missing some arguments. You may need a newer version of trace "         \
      "processor to parse them.")
// clang-format on

enum Type {
  kSingle,  // Single-value property, one value per key.
  kIndexed  // Indexed property, multiple value per key (e.g. cpu_stats[1]).
};

enum Severity {
  kInfo,      // Diagnostic counters
  kDataLoss,  // Correct operation that still resulted in data loss
  kError      // If any kError counter is > 0 trace_processor_shell will
              // raise an error. This is also surfaced in the web UI.
};

enum Source {
  // The counter is collected when recording the trace on-device and is just
  // being reflected in the stats table.
  kTrace,

  // The counter is genrated when importing / processing the trace in the trace
  // processor.
  kAnalysis
};

// Ignore GCC warning about a missing argument for a variadic macro parameter.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC system_header
#endif

// Declares an enum of literals (one for each stat). The enum values of each
// literal corresponds to the string index in the arrays below.
#define PERFETTO_TP_STATS_ENUM(name, ...) name
enum KeyIDs : size_t { PERFETTO_TP_STATS(PERFETTO_TP_STATS_ENUM), kNumKeys };

// The code below declares an array for each property (name, type, ...).

#define PERFETTO_TP_STATS_NAME(name, ...) #name
constexpr char const* kNames[] = {PERFETTO_TP_STATS(PERFETTO_TP_STATS_NAME)};

#define PERFETTO_TP_STATS_TYPE(_, type, ...) type
constexpr Type kTypes[] = {PERFETTO_TP_STATS(PERFETTO_TP_STATS_TYPE)};

#define PERFETTO_TP_STATS_SEVERITY(_, __, severity, ...) severity
constexpr Severity kSeverities[] = {
    PERFETTO_TP_STATS(PERFETTO_TP_STATS_SEVERITY)};

#define PERFETTO_TP_STATS_SOURCE(_, __, ___, source, ...) source
constexpr Source kSources[] = {PERFETTO_TP_STATS(PERFETTO_TP_STATS_SOURCE)};

#define PERFETTO_TP_STATS_DESCRIPTION(_, __, ___, ____, descr, ...) descr
constexpr char const* kDescriptions[] = {
    PERFETTO_TP_STATS(PERFETTO_TP_STATS_DESCRIPTION)};

}  // namespace stats
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_STORAGE_STATS_H_
