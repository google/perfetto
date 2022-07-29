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

#include "perfetto/profiling/pprof_builder.h"

#include "perfetto/base/build_config.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <cxxabi.h>
#endif

#include <algorithm>
#include <cinttypes>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/profiling/profile_builder.h"

// Quick hint on navigating the file:
// Conversions for both perf and heap profiles start with |TraceToPprof|.
// Non-shared logic is in the |heap_profile| and |perf_profile| namespaces.
//
// To build one or more profiles, first the callstack information is queried
// from the SQL tables, and converted into an in-memory representation by
// |PreprocessLocations|. Then an instance of |GProfileBuilder| is used to
// accumulate samples for that profile, and emit all additional information as a
// serialized proto. Only the entities referenced by that particular
// |GProfileBuilder| instance are emitted.
//
// See protos/third_party/pprof/profile.proto for the meaning of terms like
// function/location/line.


namespace perfetto {
namespace trace_to_text {
namespace {

using ::perfetto::profiling::GProfileBuilder;
using ::perfetto::trace_processor::Iterator;

std::string AsCsvString(std::vector<uint64_t> vals) {
  std::string ret;
  for (size_t i = 0; i < vals.size(); i++) {
    if (i != 0) {
      ret += ",";
    }
    ret += std::to_string(vals[i]);
  }
  return ret;
}

base::Optional<int64_t> GetStatsEntry(
    trace_processor::TraceProcessor* tp,
    const std::string& name,
    base::Optional<uint64_t> idx = base::nullopt) {
  std::string query = "select value from stats where name == '" + name + "'";
  if (idx.has_value())
    query += " and idx == " + std::to_string(idx.value());

  auto it = tp->ExecuteQuery(query);
  if (!it.Next()) {
    if (!it.Status().ok()) {
      PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                              it.Status().message().c_str());
      return base::nullopt;
    }
    // some stats are not present unless non-zero
    return base::make_optional(0);
  }
  return base::make_optional(it.Get(0).AsLong());
}

namespace heap_profile {
struct View {
  const char* type;
  const char* unit;
  const char* aggregator;
  const char* filter;
};
const View kSpaceView{"space", "bytes", "SUM(size)", nullptr};
const View kAllocSpaceView{"alloc_space", "bytes", "SUM(size)", "size >= 0"};
const View kAllocObjectsView{"alloc_objects", "count", "sum(count)",
                             "size >= 0"};
const View kObjectsView{"objects", "count", "SUM(count)", nullptr};

const View kViews[] = {kAllocObjectsView, kObjectsView, kAllocSpaceView,
                       kSpaceView};

static bool VerifyPIDStats(trace_processor::TraceProcessor* tp, uint64_t pid) {
  bool success = true;
  base::Optional<int64_t> stat =
      GetStatsEntry(tp, "heapprofd_buffer_corrupted", base::make_optional(pid));
  if (!stat.has_value()) {
    PERFETTO_DFATAL_OR_ELOG("Failed to get heapprofd_buffer_corrupted stat");
  } else if (stat.value() > 0) {
    success = false;
    PERFETTO_ELOG("WARNING: The profile for %" PRIu64
                  " ended early due to a buffer corruption."
                  " THIS IS ALWAYS A BUG IN HEAPPROFD OR"
                  " CLIENT MEMORY CORRUPTION.",
                  pid);
  }
  stat =
      GetStatsEntry(tp, "heapprofd_buffer_overran", base::make_optional(pid));
  if (!stat.has_value()) {
    PERFETTO_DFATAL_OR_ELOG("Failed to get heapprofd_buffer_overran stat");
  } else if (stat.value() > 0) {
    success = false;
    PERFETTO_ELOG("WARNING: The profile for %" PRIu64
                  " ended early due to a buffer overrun.",
                  pid);
  }

  stat = GetStatsEntry(tp, "heapprofd_rejected_concurrent", pid);
  if (!stat.has_value()) {
    PERFETTO_DFATAL_OR_ELOG("Failed to get heapprofd_rejected_concurrent stat");
  } else if (stat.value() > 0) {
    success = false;
    PERFETTO_ELOG("WARNING: The profile for %" PRIu64
                  " was rejected due to a concurrent profile.",
                  pid);
  }
  return success;
}

static std::vector<Iterator> BuildViewIterators(
    trace_processor::TraceProcessor* tp,
    uint64_t upid,
    uint64_t ts,
    const char* heap_name) {
  std::vector<Iterator> view_its;
  for (size_t i = 0; i < base::ArraySize(kViews); ++i) {
    const View& v = kViews[i];
    std::string query = "SELECT hpa.callsite_id ";
    query +=
        ", " + std::string(v.aggregator) + " FROM heap_profile_allocation hpa ";
    // TODO(fmayer): Figure out where negative callsite_id comes from.
    query += "WHERE hpa.callsite_id >= 0 ";
    query += "AND hpa.upid = " + std::to_string(upid) + " ";
    query += "AND hpa.ts <= " + std::to_string(ts) + " ";
    query += "AND hpa.heap_name = '" + std::string(heap_name) + "' ";
    if (v.filter)
      query += "AND " + std::string(v.filter) + " ";
    query += "GROUP BY hpa.callsite_id;";
    view_its.emplace_back(tp->ExecuteQuery(query));
  }
  return view_its;
}

static bool WriteAllocations(GProfileBuilder* builder,
                             std::vector<Iterator>* view_its) {
  for (;;) {
    bool all_next = true;
    bool any_next = false;
    for (size_t i = 0; i < base::ArraySize(kViews); ++i) {
      Iterator& it = (*view_its)[i];
      bool next = it.Next();
      if (!it.Status().ok()) {
        PERFETTO_DFATAL_OR_ELOG("Invalid view iterator: %s",
                                it.Status().message().c_str());
        return false;
      }
      all_next = all_next && next;
      any_next = any_next || next;
    }

    if (!all_next) {
      PERFETTO_CHECK(!any_next);
      break;
    }

    protozero::PackedVarInt sample_values;
    int64_t callstack_id = -1;
    for (size_t i = 0; i < base::ArraySize(kViews); ++i) {
      if (i == 0) {
        callstack_id = (*view_its)[i].Get(0).AsLong();
      } else if (callstack_id != (*view_its)[i].Get(0).AsLong()) {
        PERFETTO_DFATAL_OR_ELOG("Wrong callstack.");
        return false;
      }
      sample_values.Append((*view_its)[i].Get(1).AsLong());
    }

    if (!builder->AddSample(sample_values, callstack_id))
      return false;
  }
  return true;
}

static bool TraceToHeapPprof(trace_processor::TraceProcessor* tp,
                             std::vector<SerializedProfile>* output,
                             bool annotate_frames,
                             uint64_t target_pid,
                             const std::vector<uint64_t>& target_timestamps) {
  GProfileBuilder builder(tp, annotate_frames);
  bool any_fail = false;
  Iterator it = tp->ExecuteQuery(
      "select distinct hpa.upid, hpa.ts, p.pid, hpa.heap_name "
      "from heap_profile_allocation hpa, "
      "process p where p.upid = hpa.upid;");
  while (it.Next()) {
    builder.Reset();
    uint64_t upid = static_cast<uint64_t>(it.Get(0).AsLong());
    uint64_t ts = static_cast<uint64_t>(it.Get(1).AsLong());
    uint64_t profile_pid = static_cast<uint64_t>(it.Get(2).AsLong());
    const char* heap_name = it.Get(3).AsString();
    if ((target_pid > 0 && profile_pid != target_pid) ||
        (!target_timestamps.empty() &&
         std::find(target_timestamps.begin(), target_timestamps.end(), ts) ==
             target_timestamps.end())) {
      continue;
    }

    if (!VerifyPIDStats(tp, profile_pid))
      any_fail = true;

    std::vector<std::pair<std::string, std::string>> sample_types;
    for (size_t i = 0; i < base::ArraySize(kViews); ++i) {
      sample_types.emplace_back(std::string(kViews[i].type),
                                std::string(kViews[i].unit));
    }
    builder.WriteSampleTypes(sample_types);

    std::vector<Iterator> view_its =
        BuildViewIterators(tp, upid, ts, heap_name);
    std::string profile_proto;
    if (WriteAllocations(&builder, &view_its)) {
      profile_proto = builder.CompleteProfile();
    }
    output->emplace_back(
        SerializedProfile{ProfileType::kHeapProfile, profile_pid,
                          std::move(profile_proto), heap_name});
  }

  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            it.Status().message().c_str());
    return false;
  }
  if (any_fail) {
    PERFETTO_ELOG(
        "One or more of your profiles had an issue. Please consult "
        "https://perfetto.dev/docs/data-sources/"
        "native-heap-profiler#troubleshooting");
  }
  return true;
}
}  // namespace heap_profile

namespace perf_profile {
struct ProcessInfo {
  uint64_t pid;
  std::vector<uint64_t> utids;
};

// Returns a map of upid -> {pid, utids[]} for sampled processes.
static std::map<uint64_t, ProcessInfo> GetProcessMap(
    trace_processor::TraceProcessor* tp) {
  Iterator it = tp->ExecuteQuery(
      "select distinct process.upid, process.pid, thread.utid from perf_sample "
      "join thread using (utid) join process using (upid) where callsite_id is "
      "not null order by process.upid asc");
  std::map<uint64_t, ProcessInfo> process_map;
  while (it.Next()) {
    uint64_t upid = static_cast<uint64_t>(it.Get(0).AsLong());
    uint64_t pid = static_cast<uint64_t>(it.Get(1).AsLong());
    uint64_t utid = static_cast<uint64_t>(it.Get(2).AsLong());
    process_map[upid].pid = pid;
    process_map[upid].utids.push_back(utid);
  }
  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            it.Status().message().c_str());
    return {};
  }
  return process_map;
}

static void LogTracePerfEventIssues(trace_processor::TraceProcessor* tp) {
  base::Optional<int64_t> stat = GetStatsEntry(tp, "perf_samples_skipped");
  if (!stat.has_value()) {
    PERFETTO_DFATAL_OR_ELOG("Failed to look up perf_samples_skipped stat");
  } else if (stat.value() > 0) {
    PERFETTO_ELOG(
        "Warning: the trace recorded %" PRIi64
        " skipped samples, which otherwise matched the tracing config. This "
        "would cause a process to be completely absent from the trace, but "
        "does *not* imply data loss in any of the output profiles.",
        stat.value());
  }

  stat = GetStatsEntry(tp, "perf_samples_skipped_dataloss");
  if (!stat.has_value()) {
    PERFETTO_DFATAL_OR_ELOG(
        "Failed to look up perf_samples_skipped_dataloss stat");
  } else if (stat.value() > 0) {
    PERFETTO_ELOG("DATA LOSS: the trace recorded %" PRIi64
                  " lost perf samples (within traced_perf). This means that "
                  "the trace is missing information, but it is not known "
                  "which profile that affected.",
                  stat.value());
  }

  // Check if any per-cpu ringbuffers encountered dataloss (as recorded by the
  // kernel).
  Iterator it = tp->ExecuteQuery(
      "select idx, value from stats where name == 'perf_cpu_lost_records' and "
      "value > 0 order by idx asc");
  while (it.Next()) {
    PERFETTO_ELOG(
        "DATA LOSS: during the trace, the per-cpu kernel ring buffer for cpu "
        "%" PRIi64 " recorded %" PRIi64
        " lost samples. This means that the trace is missing information, "
        "but it is not known which profile that affected.",
        static_cast<int64_t>(it.Get(0).AsLong()),
        static_cast<int64_t>(it.Get(1).AsLong()));
  }
  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            it.Status().message().c_str());
  }
}

// TODO(rsavitski): decide whether errors in |AddSample| should result in an
// empty profile (and/or whether they should make the overall conversion
// unsuccessful). Furthermore, clarify the return value's semantics for both
// perf and heap profiles.
static bool TraceToPerfPprof(trace_processor::TraceProcessor* tp,
                             std::vector<SerializedProfile>* output,
                             bool annotate_frames,
                             uint64_t target_pid) {
  GProfileBuilder builder(tp, annotate_frames);

  LogTracePerfEventIssues(tp);

  // Aggregate samples by upid when building profiles.
  std::map<uint64_t, ProcessInfo> process_map = GetProcessMap(tp);
  for (const auto& p : process_map) {
    const ProcessInfo& process = p.second;

    if (target_pid != 0 && process.pid != target_pid)
      continue;

    builder.Reset();
    builder.WriteSampleTypes({{"samples", "count"}});

    std::string query = "select callsite_id from perf_sample where utid in (" +
                        AsCsvString(process.utids) +
                        ") and callsite_id is not null order by ts asc;";

    protozero::PackedVarInt single_count_value;
    single_count_value.Append(1);

    Iterator it = tp->ExecuteQuery(query);
    while (it.Next()) {
      int64_t callsite_id = static_cast<int64_t>(it.Get(0).AsLong());
      builder.AddSample(single_count_value, callsite_id);
    }
    if (!it.Status().ok()) {
      PERFETTO_DFATAL_OR_ELOG("Failed to iterate over samples: %s",
                              it.Status().c_message());
      return false;
    }

    std::string profile_proto = builder.CompleteProfile();
    output->emplace_back(SerializedProfile{
        ProfileType::kPerfProfile, process.pid, std::move(profile_proto), ""});
  }
  return true;
}
}  // namespace perf_profile
}  // namespace

bool TraceToPprof(trace_processor::TraceProcessor* tp,
                  std::vector<SerializedProfile>* output,
                  ConversionMode mode,
                  uint64_t flags,
                  uint64_t pid,
                  const std::vector<uint64_t>& timestamps) {
  bool annotate_frames =
      flags & static_cast<uint64_t>(ConversionFlags::kAnnotateFrames);
  switch (mode) {
    case (ConversionMode::kHeapProfile):
      return heap_profile::TraceToHeapPprof(tp, output, annotate_frames, pid,
                                            timestamps);
    case (ConversionMode::kPerfProfile):
      return perf_profile::TraceToPerfPprof(tp, output, annotate_frames, pid);
  }
  PERFETTO_FATAL("unknown conversion option");  // for gcc
}

}  // namespace trace_to_text
}  // namespace perfetto
