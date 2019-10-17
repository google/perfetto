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

#include "perfetto/base/build_config.h"
#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)

#include <inttypes.h>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <stdio.h>
#include <cstring>
#include <vector>

#include "perfetto/ext/base/string_splitter.h"
#include "src/trace_processor/export_json.h"
#include "src/trace_processor/metadata.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {
namespace json {

namespace {

using IndexMap = perfetto::trace_processor::TraceStorage::Stats::IndexMap;

const char kLegacyEventArgsKey[] = "legacy_event";
const char kLegacyEventCategoryKey[] = "category";
const char kLegacyEventNameKey[] = "name";
const char kLegacyEventPhaseKey[] = "phase";
const char kLegacyEventDurationNsKey[] = "duration_ns";
const char kLegacyEventThreadTimestampNsKey[] = "thread_timestamp_ns";
const char kLegacyEventThreadDurationNsKey[] = "thread_duration_ns";
const char kLegacyEventThreadInstructionCountKey[] = "thread_instruction_count";
const char kLegacyEventThreadInstructionDeltaKey[] = "thread_instruction_delta";
const char kLegacyEventUseAsyncTtsKey[] = "use_async_tts";
const char kLegacyEventUnscopedIdKey[] = "unscoped_id";
const char kLegacyEventGlobalIdKey[] = "global_id";
const char kLegacyEventLocalIdKey[] = "local_id";
const char kLegacyEventIdScopeKey[] = "id_scope";
const char kLegacyEventBindIdKey[] = "bind_id";
const char kLegacyEventBindToEnclosingKey[] = "bind_to_enclosing";
const char kLegacyEventFlowDirectionKey[] = "flow_direction";
const char kFlowDirectionValueIn[] = "in";
const char kFlowDirectionValueOut[] = "out";
const char kFlowDirectionValueInout[] = "inout";

const char* GetNonNullString(const TraceStorage* storage, StringId id) {
  return id == kNullStringId ? "" : storage->GetString(id).c_str();
}

class TraceFormatWriter {
 public:
  TraceFormatWriter(FILE* output) : output_(output), first_event_(true) {
    WriteHeader();
  }

  ~TraceFormatWriter() { WriteFooter(); }

  void WriteCommonEvent(const Json::Value& event) {
    if (!first_event_) {
      fputs(",", output_);
    }
    Json::FastWriter writer;
    fputs(writer.write(event).c_str(), output_);
    first_event_ = false;
  }

  void WriteMetadataEvent(const char* metadata_type,
                          const char* metadata_value,
                          uint32_t tid,
                          uint32_t pid) {
    if (!first_event_) {
      fputs(",", output_);
    }
    Json::FastWriter writer;
    Json::Value value;
    value["ph"] = "M";
    value["cat"] = "__metadata";
    value["ts"] = 0;
    value["name"] = metadata_type;
    value["tid"] = Json::UInt(tid);
    value["pid"] = Json::UInt(pid);

    Json::Value args;
    args["name"] = metadata_value;
    value["args"] = args;

    fputs(writer.write(value).c_str(), output_);
    first_event_ = false;
  }

  void MergeMetadata(const Json::Value& value) {
    for (const auto& member : value.getMemberNames()) {
      metadata_[member] = value[member];
    }
  }

  void AppendTelemetryMetadataString(const char* key, const char* value) {
    metadata_["telemetry"][key].append(value);
  }

  void AppendTelemetryMetadataInt(const char* key, int64_t value) {
    metadata_["telemetry"][key].append(Json::Int64(value));
  }

  void AppendTelemetryMetadataBool(const char* key, bool value) {
    metadata_["telemetry"][key].append(value);
  }

  void SetTelemetryMetadataTimestamp(const char* key, int64_t value) {
    metadata_["telemetry"][key] = value / 1000.0;
  }

  void SetPerfettoStats(const char* key, int64_t value) {
    metadata_["perfetto_trace_stats"][key] = Json::Int64(value);
  }

  void SetPerfettoBufferStats(const char* key, const IndexMap& indexed_values) {
    for (const auto& value : indexed_values) {
      metadata_["perfetto_trace_stats"]["buffer_stats"][value.first][key] =
          Json::Int64(value.second);
    }
  }

  void AddSystemTraceData(const std::string& data) {
    system_trace_data_ += data;
  }

  void AddUserTraceData(const std::string& data) { user_trace_data_ += data; }

 private:
  void WriteHeader() { fputs("{\"traceEvents\":[\n", output_); }

  void WriteFooter() {
    Json::FastWriter writer;
    if (!user_trace_data_.empty()) {
      Json::Reader reader;
      Json::Value result;
      if (reader.parse(user_trace_data_, result)) {
        WriteCommonEvent(result);
      } else {
        PERFETTO_DLOG(
            "can't parse legacy user json trace export, skipping. data: %s",
            user_trace_data_.c_str());
      }
    }
    fputs("]", output_);
    if (!system_trace_data_.empty()) {
      fputs(",\"systemTraceEvents\":\n", output_);
      fputs(writer.write(Json::Value(system_trace_data_)).c_str(), output_);
    }
    if (!metadata_.empty()) {
      fputs(",\"metadata\":\n", output_);
      fputs(writer.write(metadata_).c_str(), output_);
    }
    fputs("}", output_);
    fflush(output_);
  }

  FILE* output_;
  bool first_event_;
  Json::Value metadata_;
  std::string system_trace_data_;
  std::string user_trace_data_;
};

std::string PrintUint64(uint64_t x) {
  char hex_str[19];
  sprintf(hex_str, "0x%" PRIx64, x);
  return hex_str;
}

class ArgsBuilder {
 public:
  explicit ArgsBuilder(const TraceStorage* storage) : storage_(storage) {
    const TraceStorage::Args& args = storage->args();
    Json::Value empty_value(Json::objectValue);
    if (args.args_count() == 0) {
      args_sets_.resize(1, empty_value);
      return;
    }
    args_sets_.resize(args.set_ids().back() + 1, empty_value);
    for (size_t i = 0; i < args.args_count(); ++i) {
      ArgSetId set_id = args.set_ids()[i];
      const char* key = GetNonNullString(storage_, args.keys()[i]);
      Variadic value = args.arg_values()[i];
      AppendArg(set_id, key, VariadicToJson(value));
    }
    PostprocessArgs();
  }

  const Json::Value& GetArgs(ArgSetId set_id) const {
    return args_sets_[set_id];
  }

 private:
  Json::Value VariadicToJson(Variadic variadic) {
    switch (variadic.type) {
      case Variadic::kInt:
        return Json::Int64(variadic.int_value);
      case Variadic::kUint:
        return Json::UInt64(variadic.uint_value);
      case Variadic::kString:
        return GetNonNullString(storage_, variadic.string_value);
      case Variadic::kReal:
        return variadic.real_value;
      case Variadic::kPointer:
        return PrintUint64(variadic.pointer_value);
      case Variadic::kBool:
        return variadic.bool_value;
      case Variadic::kJson:
        Json::Reader reader;
        Json::Value result;
        reader.parse(GetNonNullString(storage_, variadic.json_value), result);
        return result;
    }
    PERFETTO_FATAL("Not reached");  // For gcc.
  }

  void AppendArg(ArgSetId set_id,
                 const std::string& key,
                 const Json::Value& value) {
    Json::Value* target = &args_sets_[set_id];
    for (base::StringSplitter parts(key, '.'); parts.Next();) {
      std::string key_part = parts.cur_token();
      size_t bracketpos = key_part.find('[');
      if (bracketpos == key_part.npos) {  // A single item
        target = &(*target)[key_part];
      } else {  // A list item
        target = &(*target)[key_part.substr(0, bracketpos)];
        while (bracketpos != key_part.npos) {
          std::string index = key_part.substr(
              bracketpos + 1, key_part.find(']', bracketpos) - bracketpos - 1);
          target = &(*target)[stoi(index)];
          bracketpos = key_part.find('[', bracketpos + 1);
        }
      }
    }
    *target = value;
  }

  void PostprocessArgs() {
    for (Json::Value& args : args_sets_) {
      // Move all fields from "debug" key to upper level.
      if (args.isMember("debug")) {
        Json::Value debug = args["debug"];
        args.removeMember("debug");
        for (const auto& member : debug.getMemberNames()) {
          args[member] = debug[member];
        }
      }

      // Rename source fields.
      if (args.isMember("task")) {
        if (args["task"].isMember("posted_from")) {
          Json::Value posted_from = args["task"]["posted_from"];
          args["task"].removeMember("posted_from");
          if (posted_from.isMember("function_name")) {
            args["src_func"] = posted_from["function_name"];
            args["src_file"] = posted_from["file_name"];
          } else if (posted_from.isMember("file_name")) {
            args["src"] = posted_from["file_name"];
          }
        }
        if (args["task"].empty()) {
          args.removeMember("task");
        }
      }
    }
  }

  const TraceStorage* storage_;
  std::vector<Json::Value> args_sets_;
};

void ConvertLegacyFlowEventArgs(const Json::Value& legacy_args,
                                Json::Value* event) {
  if (legacy_args.isMember(kLegacyEventIdScopeKey))
    (*event)["scope"] = legacy_args[kLegacyEventIdScopeKey];

  if (legacy_args.isMember(kLegacyEventBindIdKey)) {
    (*event)["bind_id"] =
        PrintUint64(legacy_args[kLegacyEventBindIdKey].asUInt64());
  }

  if (legacy_args.isMember(kLegacyEventBindToEnclosingKey))
    (*event)["bp"] = "e";

  if (legacy_args.isMember(kLegacyEventFlowDirectionKey)) {
    const char* val = legacy_args[kLegacyEventFlowDirectionKey].asCString();
    if (strcmp(val, kFlowDirectionValueIn) == 0) {
      (*event)["flow_in"] = true;
    } else if (strcmp(val, kFlowDirectionValueOut) == 0) {
      (*event)["flow_out"] = true;
    } else {
      PERFETTO_DCHECK(strcmp(val, kFlowDirectionValueInout) == 0);
      (*event)["flow_in"] = true;
      (*event)["flow_out"] = true;
    }
  }
}

ResultCode ExportThreadNames(const TraceStorage* storage,
                             TraceFormatWriter* writer) {
  for (UniqueTid i = 1; i < storage->thread_count(); ++i) {
    auto thread = storage->GetThread(i);
    if (!thread.name_id.is_null()) {
      const char* thread_name = GetNonNullString(storage, thread.name_id);
      uint32_t pid = thread.upid ? storage->GetProcess(*thread.upid).pid : 0;
      writer->WriteMetadataEvent("thread_name", thread_name, thread.tid, pid);
    }
  }
  return kResultOk;
}

ResultCode ExportProcessNames(const TraceStorage* storage,
                              TraceFormatWriter* writer) {
  for (UniquePid i = 1; i < storage->process_count(); ++i) {
    auto process = storage->GetProcess(i);
    if (!process.name_id.is_null()) {
      const char* process_name = GetNonNullString(storage, process.name_id);
      writer->WriteMetadataEvent("process_name", process_name, 0, process.pid);
    }
  }
  return kResultOk;
}

ResultCode ExportSlices(const TraceStorage* storage,
                        const ArgsBuilder& args_builder,
                        TraceFormatWriter* writer) {
  const auto& slices = storage->nestable_slices();
  for (uint32_t i = 0; i < slices.slice_count(); ++i) {
    Json::Value event;
    event["ts"] = Json::Int64(slices.start_ns()[i] / 1000);
    event["cat"] = GetNonNullString(storage, slices.categories()[i]);
    event["name"] = GetNonNullString(storage, slices.names()[i]);
    event["pid"] = 0;
    const Json::Value& args = args_builder.GetArgs(slices.arg_set_ids()[i]);
    if (!args.empty()) {
      event["args"] = args;  // Makes a copy of |args|.

      if (event["args"].isMember(kLegacyEventArgsKey)) {
        ConvertLegacyFlowEventArgs(event["args"][kLegacyEventArgsKey], &event);

        if (event["args"].empty())
          event.removeMember("args");
      }
    }

    if (slices.types()[i] == RefType::kRefTrack) {  // Async event.
      TrackId track_id = static_cast<TrackId>(slices.refs()[i]);

      // TODO(lalitm): add a check here for looking up source_arg_set_id
      // and checking that this track originated from Chrome.

      const auto& process_track = storage->process_track_table();
      auto opt_process_row =
          process_track.id().IndexOf(SqlValue::Long(track_id));
      if (opt_process_row.has_value()) {
        uint32_t upid = process_track.upid()[*opt_process_row];
        event["id2"]["local"] = PrintUint64(track_id);
        event["pid"] = storage->GetProcess(upid).pid;
      } else {
        event["id2"]["global"] = PrintUint64(track_id);
      }

      const auto& virtual_track_slices = storage->virtual_track_slices();
      int64_t thread_ts_ns = 0;
      int64_t thread_duration_ns = 0;
      int64_t thread_instruction_count = 0;
      int64_t thread_instruction_delta = 0;
      base::Optional<uint32_t> vtrack_slice_row =
          virtual_track_slices.FindRowForSliceId(i);
      if (vtrack_slice_row) {
        thread_ts_ns =
            virtual_track_slices.thread_timestamp_ns()[*vtrack_slice_row];
        thread_duration_ns =
            virtual_track_slices.thread_duration_ns()[*vtrack_slice_row];
        thread_instruction_count =
            virtual_track_slices.thread_instruction_counts()[*vtrack_slice_row];
        thread_instruction_delta =
            virtual_track_slices.thread_instruction_deltas()[*vtrack_slice_row];
      }

      if (thread_ts_ns > 0) {
        event["tts"] = Json::Int64(thread_ts_ns / 1000);
        event["use_async_tts"] = Json::Int(1);
      }
      if (thread_instruction_count > 0) {
        event["ticount"] = Json::Int64(thread_instruction_count);
        event["use_async_tts"] = Json::Int(1);
      }

      int64_t duration_ns = slices.durations()[i];
      if (duration_ns == 0) {  // Instant async event.
        event["ph"] = "n";
        writer->WriteCommonEvent(event);
      } else {  // Async start and end.
        event["ph"] = "b";
        writer->WriteCommonEvent(event);
        // If the slice didn't finish, the duration may be negative. Don't
        // write the end event in this case.
        if (duration_ns > 0) {
          event["ph"] = "e";
          event["ts"] =
              Json::Int64((slices.start_ns()[i] + duration_ns) / 1000);
          if (thread_ts_ns > 0) {
            event["tts"] =
                Json::Int64((thread_ts_ns + thread_duration_ns) / 1000);
          }
          if (thread_instruction_count > 0) {
            event["ticount"] = Json::Int64(
                (thread_instruction_count + thread_instruction_delta));
          }
          event.removeMember("args");
          writer->WriteCommonEvent(event);
        }
      }
    } else {  // Sync event.
      const auto& thread_slices = storage->thread_slices();
      int64_t thread_ts_ns = 0;
      int64_t thread_duration_ns = 0;
      int64_t thread_instruction_count = 0;
      int64_t thread_instruction_delta = 0;
      base::Optional<uint32_t> thread_slice_row =
          thread_slices.FindRowForSliceId(i);
      if (thread_slice_row) {
        thread_ts_ns = thread_slices.thread_timestamp_ns()[*thread_slice_row];
        thread_duration_ns =
            thread_slices.thread_duration_ns()[*thread_slice_row];
        thread_instruction_count =
            thread_slices.thread_instruction_counts()[*thread_slice_row];
        thread_instruction_delta =
            thread_slices.thread_instruction_deltas()[*thread_slice_row];
      }
      int64_t duration_ns = slices.durations()[i];
      if (duration_ns == 0) {  // Instant event.
        event["ph"] = "i";
        if (slices.types()[i] == RefType::kRefUtid) {
          UniqueTid utid = static_cast<UniqueTid>(slices.refs()[i]);
          auto thread = storage->GetThread(utid);
          if (thread.upid) {
            event["pid"] = storage->GetProcess(*thread.upid).pid;
          }
          if (thread_ts_ns > 0) {
            event["tts"] = Json::Int64(thread_ts_ns / 1000);
          }
          if (thread_instruction_count > 0) {
            event["ticount"] = Json::Int64(thread_instruction_count);
          }
          event["tid"] = thread.tid;
          event["s"] = "t";
        } else if (slices.types()[i] == RefType::kRefUpid) {
          UniquePid upid = static_cast<UniquePid>(slices.refs()[i]);
          event["pid"] = storage->GetProcess(upid).pid;
          event["s"] = "p";
        } else if (slices.types()[i] == RefType::kRefNoRef) {
          event["s"] = "g";
        } else {
          return kResultWrongRefType;
        }
        writer->WriteCommonEvent(event);
      } else {  // Complete event.
        if (slices.types()[i] != RefType::kRefUtid) {
          return kResultWrongRefType;
        }
        if (duration_ns > 0) {
          event["ph"] = "X";
          event["dur"] = Json::Int64(duration_ns / 1000);
        } else {
          // If the slice didn't finish, the duration may be negative. Only
          // write a begin event without end event in this case.
          event["ph"] = "B";
        }
        UniqueTid utid = static_cast<UniqueTid>(slices.refs()[i]);
        auto thread = storage->GetThread(utid);
        event["tid"] = thread.tid;
        if (thread.upid) {
          event["pid"] = storage->GetProcess(*thread.upid).pid;
        }
        if (thread_ts_ns > 0) {
          event["tts"] = Json::Int64(thread_ts_ns / 1000);
          // Only write thread duration for completed events.
          if (duration_ns > 0)
            event["tdur"] = Json::Int64(thread_duration_ns / 1000);
        }
        if (thread_instruction_count > 0) {
          event["ticount"] = Json::Int64(thread_instruction_count);
          // Only write thread instruction delta for completed events.
          if (duration_ns > 0)
            event["tidelta"] = Json::Int64(thread_instruction_delta);
        }
        writer->WriteCommonEvent(event);
      }
    }
  }
  return kResultOk;
}

Json::Value ConvertLegacyRawEventToJson(const TraceStorage* storage,
                                        const ArgsBuilder& args_builder,
                                        uint32_t index) {
  const auto& events = storage->raw_events();

  Json::Value event;
  event["ts"] = Json::Int64(events.timestamps()[index] / 1000);

  UniqueTid utid = static_cast<UniqueTid>(events.utids()[index]);
  auto thread = storage->GetThread(utid);
  event["tid"] = thread.tid;
  if (thread.upid) {
    event["pid"] = storage->GetProcess(*thread.upid).pid;
  }

  // Raw legacy events store all other params in the arg set. Make a copy of
  // the converted args here, parse, and then remove the legacy params.
  event["args"] = args_builder.GetArgs(events.arg_set_ids()[index]);
  const Json::Value& legacy_args = event["args"][kLegacyEventArgsKey];

  PERFETTO_DCHECK(legacy_args.isMember(kLegacyEventCategoryKey));
  event["cat"] = legacy_args[kLegacyEventCategoryKey];

  PERFETTO_DCHECK(legacy_args.isMember(kLegacyEventNameKey));
  event["name"] = legacy_args[kLegacyEventNameKey];

  PERFETTO_DCHECK(legacy_args.isMember(kLegacyEventPhaseKey));
  event["ph"] = legacy_args[kLegacyEventPhaseKey];

  if (legacy_args.isMember(kLegacyEventDurationNsKey))
    event["dur"] = legacy_args[kLegacyEventDurationNsKey].asInt64() / 1000;

  if (legacy_args.isMember(kLegacyEventThreadTimestampNsKey)) {
    event["tts"] =
        legacy_args[kLegacyEventThreadTimestampNsKey].asInt64() / 1000;
  }

  if (legacy_args.isMember(kLegacyEventThreadDurationNsKey)) {
    event["tdur"] =
        legacy_args[kLegacyEventThreadDurationNsKey].asInt64() / 1000;
  }

  if (legacy_args.isMember(kLegacyEventThreadInstructionCountKey))
    event["ticount"] = legacy_args[kLegacyEventThreadInstructionCountKey];

  if (legacy_args.isMember(kLegacyEventThreadInstructionDeltaKey))
    event["tidelta"] = legacy_args[kLegacyEventThreadInstructionDeltaKey];

  if (legacy_args.isMember(kLegacyEventUseAsyncTtsKey))
    event["use_async_tts"] = legacy_args[kLegacyEventUseAsyncTtsKey];

  if (legacy_args.isMember(kLegacyEventUnscopedIdKey)) {
    event["id"] =
        PrintUint64(legacy_args[kLegacyEventUnscopedIdKey].asUInt64());
  }

  if (legacy_args.isMember(kLegacyEventGlobalIdKey)) {
    event["id2"]["global"] =
        PrintUint64(legacy_args[kLegacyEventGlobalIdKey].asUInt64());
  }

  if (legacy_args.isMember(kLegacyEventLocalIdKey)) {
    event["id2"]["local"] =
        PrintUint64(legacy_args[kLegacyEventLocalIdKey].asUInt64());
  }

  ConvertLegacyFlowEventArgs(legacy_args, &event);

  event["args"].removeMember(kLegacyEventArgsKey);
  if (event["args"].empty())
    event.removeMember("args");

  return event;
}

ResultCode ExportRawEvents(const TraceStorage* storage,
                           const ArgsBuilder& args_builder,
                           TraceFormatWriter* writer) {
  base::Optional<StringId> raw_legacy_event_key_id =
      storage->string_pool().GetId("track_event.legacy_event");
  base::Optional<StringId> raw_legacy_system_trace_event_id =
      storage->string_pool().GetId("chrome_event.legacy_system_trace");
  base::Optional<StringId> raw_legacy_user_trace_event_id =
      storage->string_pool().GetId("chrome_event.legacy_user_trace");
  base::Optional<StringId> raw_chrome_metadata_event_id =
      storage->string_pool().GetId("chrome_event.metadata");

  const auto& events = storage->raw_events();
  for (uint32_t i = 0; i < events.raw_event_count(); ++i) {
    if (raw_legacy_event_key_id &&
        events.name_ids()[i] == *raw_legacy_event_key_id) {
      Json::Value event = ConvertLegacyRawEventToJson(storage, args_builder, i);
      writer->WriteCommonEvent(event);
    } else if (raw_legacy_system_trace_event_id &&
               events.name_ids()[i] == *raw_legacy_system_trace_event_id) {
      Json::Value args = args_builder.GetArgs(events.arg_set_ids()[i]);
      PERFETTO_DCHECK(args.isMember("data"));
      writer->AddSystemTraceData(args["data"].asString());
    } else if (raw_legacy_user_trace_event_id &&
               events.name_ids()[i] == *raw_legacy_user_trace_event_id) {
      Json::Value args = args_builder.GetArgs(events.arg_set_ids()[i]);
      PERFETTO_DCHECK(args.isMember("data"));
      writer->AddUserTraceData(args["data"].asString());
    } else if (raw_chrome_metadata_event_id &&
               events.name_ids()[i] == *raw_chrome_metadata_event_id) {
      Json::Value args = args_builder.GetArgs(events.arg_set_ids()[i]);
      writer->MergeMetadata(args);
    }
  }
  return kResultOk;
}

ResultCode ExportCpuProfileSamples(const TraceStorage* storage,
                                   TraceFormatWriter* writer) {
  const TraceStorage::CpuProfileStackSamples& samples =
      storage->cpu_profile_stack_samples();
  for (uint32_t i = 0; i < samples.size(); ++i) {
    Json::Value event;
    event["ts"] = Json::Int64(samples.timestamps()[i] / 1000);

    UniqueTid utid = static_cast<UniqueTid>(samples.utids()[i]);
    auto thread = storage->GetThread(utid);
    event["tid"] = thread.tid;
    if (thread.upid)
      event["pid"] = storage->GetProcess(*thread.upid).pid;

    event["ph"] = "I";
    event["cat"] = "disabled_by_default-cpu_profiler";
    event["name"] = "StackCpuSampling";
    event["scope"] = "t";

    std::vector<std::string> callstack;
    const TraceStorage::StackProfileCallsites& callsites =
        storage->stack_profile_callsites();
    int64_t maybe_callsite_id = samples.callsite_ids()[i];
    PERFETTO_DCHECK(maybe_callsite_id >= 0 &&
                    maybe_callsite_id < callsites.size());
    while (maybe_callsite_id >= 0) {
      size_t callsite_id = static_cast<size_t>(maybe_callsite_id);

      const TraceStorage::StackProfileFrames& frames =
          storage->stack_profile_frames();
      PERFETTO_DCHECK(callsites.frame_ids()[callsite_id] >= 0 &&
                      callsites.frame_ids()[callsite_id] < frames.size());
      size_t frame_id = static_cast<size_t>(callsites.frame_ids()[callsite_id]);

      const TraceStorage::StackProfileMappings& mappings =
          storage->stack_profile_mappings();
      PERFETTO_DCHECK(frames.mappings()[frame_id] >= 0 &&
                      frames.mappings()[frame_id] < mappings.size());
      size_t mapping_id = static_cast<size_t>(frames.mappings()[frame_id]);

      NullTermStringView symbol_name;
      uint32_t symbol_set_id = frames.symbol_set_ids()[frame_id];
      if (symbol_set_id) {
        symbol_name =
            storage->GetString(storage->symbol_table().name()[symbol_set_id]);
      }

      char frame_entry[1024];
      snprintf(
          frame_entry, sizeof(frame_entry), "%s - %s [%s]\n",
          (symbol_name.empty()
               ? PrintUint64(static_cast<uint64_t>(frames.rel_pcs()[frame_id]))
                     .c_str()
               : symbol_name.c_str()),
          GetNonNullString(storage, mappings.names()[mapping_id]),
          GetNonNullString(storage, mappings.build_ids()[mapping_id]));

      callstack.emplace_back(frame_entry);

      maybe_callsite_id = callsites.parent_callsite_ids()[callsite_id];
    }

    std::string merged_callstack;
    for (auto entry = callstack.rbegin(); entry != callstack.rend(); ++entry) {
      merged_callstack += *entry;
    }

    event["args"]["frames"] = merged_callstack;
    writer->WriteCommonEvent(event);
  }

  return kResultOk;
}

ResultCode ExportMetadata(const TraceStorage* storage,
                          TraceFormatWriter* writer) {
  const auto& trace_metadata = storage->metadata();
  const auto& keys = trace_metadata.keys();
  const auto& values = trace_metadata.values();
  for (size_t pos = 0; pos < keys.size(); pos++) {
    // Cast away from enum type, as otherwise -Wswitch-enum will demand an
    // exhaustive list of cases, even if there's a default case.
    switch (static_cast<size_t>(keys[pos])) {
      case metadata::benchmark_description:
        writer->AppendTelemetryMetadataString(
            "benchmarkDescriptions",
            GetNonNullString(storage, values[pos].string_value));
        break;

      case metadata::benchmark_name:
        writer->AppendTelemetryMetadataString(
            "benchmarks", GetNonNullString(storage, values[pos].string_value));
        break;

      case metadata::benchmark_start_time_us:

        writer->SetTelemetryMetadataTimestamp("benchmarkStart",
                                              values[pos].int_value);
        break;

      case metadata::benchmark_had_failures:
        if (pos < values.size())
          writer->AppendTelemetryMetadataBool("hadFailures",
                                              values[pos].int_value);
        break;

      case metadata::benchmark_label:
        writer->AppendTelemetryMetadataString(
            "labels", GetNonNullString(storage, values[pos].string_value));
        break;

      case metadata::benchmark_story_name:
        writer->AppendTelemetryMetadataString(
            "stories", GetNonNullString(storage, values[pos].string_value));
        break;

      case metadata::benchmark_story_run_index:
        writer->AppendTelemetryMetadataInt("storysetRepeats",
                                           values[pos].int_value);
        break;

      case metadata::benchmark_story_run_time_us:
        writer->SetTelemetryMetadataTimestamp("traceStart",
                                              values[pos].int_value);
        break;

      case metadata::benchmark_story_tags:  // repeated
        writer->AppendTelemetryMetadataString(
            "storyTags", GetNonNullString(storage, values[pos].string_value));
        break;

      default:
        PERFETTO_DLOG("Ignoring metadata key %zu",
                      static_cast<size_t>(keys[pos]));
        break;
    }
  }
  return kResultOk;
}

ResultCode ExportStats(const TraceStorage* storage, TraceFormatWriter* writer) {
  const auto& stats = storage->stats();

  writer->SetPerfettoStats("producers_connected",
                           stats[stats::traced_producers_connected].value);
  writer->SetPerfettoStats("producers_seen",
                           stats[stats::traced_producers_seen].value);
  writer->SetPerfettoStats("data_sources_registered",
                           stats[stats::traced_data_sources_registered].value);
  writer->SetPerfettoStats("data_sources_seen",
                           stats[stats::traced_data_sources_seen].value);
  writer->SetPerfettoStats("tracing_sessions",
                           stats[stats::traced_tracing_sessions].value);
  writer->SetPerfettoStats("total_buffers",
                           stats[stats::traced_total_buffers].value);
  writer->SetPerfettoStats("chunks_discarded",
                           stats[stats::traced_chunks_discarded].value);
  writer->SetPerfettoStats("patches_discarded",
                           stats[stats::traced_patches_discarded].value);

  writer->SetPerfettoBufferStats(
      "buffer_size", stats[stats::traced_buf_buffer_size].indexed_values);
  writer->SetPerfettoBufferStats(
      "bytes_written", stats[stats::traced_buf_bytes_written].indexed_values);
  writer->SetPerfettoBufferStats(
      "bytes_overwritten",
      stats[stats::traced_buf_bytes_overwritten].indexed_values);
  writer->SetPerfettoBufferStats(
      "bytes_read", stats[stats::traced_buf_bytes_read].indexed_values);
  writer->SetPerfettoBufferStats(
      "padding_bytes_written",
      stats[stats::traced_buf_padding_bytes_written].indexed_values);
  writer->SetPerfettoBufferStats(
      "padding_bytes_cleared",
      stats[stats::traced_buf_padding_bytes_cleared].indexed_values);
  writer->SetPerfettoBufferStats(
      "chunks_written", stats[stats::traced_buf_chunks_written].indexed_values);
  writer->SetPerfettoBufferStats(
      "chunks_rewritten",
      stats[stats::traced_buf_chunks_rewritten].indexed_values);
  writer->SetPerfettoBufferStats(
      "chunks_overwritten",
      stats[stats::traced_buf_chunks_overwritten].indexed_values);
  writer->SetPerfettoBufferStats(
      "chunks_discarded",
      stats[stats::traced_buf_chunks_discarded].indexed_values);
  writer->SetPerfettoBufferStats(
      "chunks_read", stats[stats::traced_buf_chunks_read].indexed_values);
  writer->SetPerfettoBufferStats(
      "chunks_committed_out_of_order",
      stats[stats::traced_buf_chunks_committed_out_of_order].indexed_values);
  writer->SetPerfettoBufferStats(
      "write_wrap_count",
      stats[stats::traced_buf_write_wrap_count].indexed_values);
  writer->SetPerfettoBufferStats(
      "patches_succeeded",
      stats[stats::traced_buf_patches_succeeded].indexed_values);
  writer->SetPerfettoBufferStats(
      "patches_failed", stats[stats::traced_buf_patches_failed].indexed_values);
  writer->SetPerfettoBufferStats(
      "readaheads_succeeded",
      stats[stats::traced_buf_readaheads_succeeded].indexed_values);
  writer->SetPerfettoBufferStats(
      "readaheads_failed",
      stats[stats::traced_buf_readaheads_failed].indexed_values);
  writer->SetPerfettoBufferStats(
      "trace_writer_packet_loss",
      stats[stats::traced_buf_trace_writer_packet_loss].indexed_values);

  return kResultOk;
}

}  // namespace

ResultCode ExportJson(const TraceStorage* storage, FILE* output) {
  TraceFormatWriter writer(output);
  ArgsBuilder args_builder(storage);

  ResultCode code = ExportThreadNames(storage, &writer);
  if (code != kResultOk)
    return code;

  code = ExportProcessNames(storage, &writer);
  if (code != kResultOk)
    return code;

  code = ExportSlices(storage, args_builder, &writer);
  if (code != kResultOk)
    return code;

  code = ExportRawEvents(storage, args_builder, &writer);
  if (code != kResultOk)
    return code;

  code = ExportCpuProfileSamples(storage, &writer);
  if (code != kResultOk)
    return code;

  code = ExportMetadata(storage, &writer);
  if (code != kResultOk)
    return code;

  code = ExportStats(storage, &writer);
  if (code != kResultOk)
    return code;

  return kResultOk;
}

}  // namespace json
}  // namespace trace_processor
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
