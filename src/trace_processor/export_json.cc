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

#include "perfetto/ext/trace_processor/export_json.h"
#include "src/trace_processor/export_json.h"

#include <stdio.h>
#include <sstream>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/trace_processor_storage_impl.h"
#include "src/trace_processor/types/trace_processor_context.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
#include <json/reader.h>
#include <json/writer.h>
#endif

namespace perfetto {
namespace trace_processor {
namespace json {

namespace {

class FileWriter : public OutputWriter {
 public:
  FileWriter(FILE* file) : file_(file) {}
  ~FileWriter() override { fflush(file_); }

  util::Status AppendString(const std::string& s) override {
    size_t written =
        fwrite(s.data(), sizeof(std::string::value_type), s.size(), file_);
    if (written != s.size())
      return util::ErrStatus("Error writing to file: %d", ferror(file_));
    return util::OkStatus();
  }

 private:
  FILE* file_;
};

#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
using IndexMap = perfetto::trace_processor::TraceStorage::Stats::IndexMap;

const char kLegacyEventArgsKey[] = "legacy_event";
const char kLegacyEventPassthroughUtidKey[] = "passthrough_utid";
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
const char kStrippedArgument[] = "__stripped__";

const char* GetNonNullString(const TraceStorage* storage,
                             base::Optional<StringId> id) {
  return id == base::nullopt || *id == kNullStringId
             ? ""
             : storage->GetString(*id).c_str();
}

class JsonExporter {
 public:
  JsonExporter(const TraceStorage* storage,
               OutputWriter* output,
               ArgumentFilterPredicate argument_filter,
               MetadataFilterPredicate metadata_filter,
               LabelFilterPredicate label_filter)
      : storage_(storage),
        args_builder_(storage_),
        writer_(output, argument_filter, metadata_filter, label_filter) {}

  util::Status Export() {
    util::Status status = MapUniquePidsAndTids();
    if (!status.ok())
      return status;

    status = ExportThreadNames();
    if (!status.ok())
      return status;

    status = ExportProcessNames();
    if (!status.ok())
      return status;

    status = ExportProcessUptimes();
    if (!status.ok())
      return status;

    status = ExportSlices();
    if (!status.ok())
      return status;

    status = ExportFlows();
    if (!status.ok())
      return status;

    status = ExportRawEvents();
    if (!status.ok())
      return status;

    status = ExportCpuProfileSamples();
    if (!status.ok())
      return status;

    status = ExportMetadata();
    if (!status.ok())
      return status;

    status = ExportStats();
    if (!status.ok())
      return status;

    status = ExportMemorySnapshots();
    if (!status.ok())
      return status;

    return util::OkStatus();
  }

 private:
  class TraceFormatWriter {
   public:
    TraceFormatWriter(OutputWriter* output,
                      ArgumentFilterPredicate argument_filter,
                      MetadataFilterPredicate metadata_filter,
                      LabelFilterPredicate label_filter)
        : output_(output),
          argument_filter_(argument_filter),
          metadata_filter_(metadata_filter),
          label_filter_(label_filter),
          first_event_(true) {
      Json::StreamWriterBuilder b;
      b.settings_["indentation"] = "";
      writer_.reset(b.newStreamWriter());
      WriteHeader();
    }

    ~TraceFormatWriter() { WriteFooter(); }

    void WriteCommonEvent(const Json::Value& event) {
      if (label_filter_ && !label_filter_("traceEvents"))
        return;

      DoWriteEvent(event);
    }

    void AddAsyncBeginEvent(const Json::Value& event) {
      if (label_filter_ && !label_filter_("traceEvents"))
        return;

      async_begin_events_.push_back(event);
    }

    void AddAsyncInstantEvent(const Json::Value& event) {
      if (label_filter_ && !label_filter_("traceEvents"))
        return;

      async_instant_events_.push_back(event);
    }

    void AddAsyncEndEvent(const Json::Value& event) {
      if (label_filter_ && !label_filter_("traceEvents"))
        return;

      async_end_events_.push_back(event);
    }

    void SortAndEmitAsyncEvents() {
      // Catapult doesn't handle out-of-order begin/end events well, especially
      // when their timestamps are the same, but their order is incorrect. Since
      // we process events sorted by begin timestamp, |async_begin_events_| and
      // |async_instant_events_| are already sorted. We now only have to sort
      // |async_end_events_| and merge-sort all events into a single sequence.

      // Sort |async_end_events_|. Note that we should order by ascending
      // timestamp, but in reverse-stable order. This way, a child slices's end
      // is emitted before its parent's end event, even if both end events have
      // the same timestamp. To accomplish this, we perform a stable sort in
      // descending order and later iterate via reverse iterators.
      struct {
        bool operator()(const Json::Value& a, const Json::Value& b) const {
          return a["ts"].asInt64() > b["ts"].asInt64();
        }
      } CompareEvents;
      std::stable_sort(async_end_events_.begin(), async_end_events_.end(),
                       CompareEvents);

      // Merge sort by timestamp. If events share the same timestamp, prefer
      // instant events, then end events, so that old slices close before new
      // ones are opened, but instant events remain in their deepest nesting
      // level.
      auto instant_event_it = async_instant_events_.begin();
      auto end_event_it = async_end_events_.rbegin();
      auto begin_event_it = async_begin_events_.begin();

      auto has_instant_event = instant_event_it != async_instant_events_.end();
      auto has_end_event = end_event_it != async_end_events_.rend();
      auto has_begin_event = begin_event_it != async_begin_events_.end();

      auto emit_next_instant = [&instant_event_it, &has_instant_event, this]() {
        DoWriteEvent(*instant_event_it);
        instant_event_it++;
        has_instant_event = instant_event_it != async_instant_events_.end();
      };
      auto emit_next_end = [&end_event_it, &has_end_event, this]() {
        DoWriteEvent(*end_event_it);
        end_event_it++;
        has_end_event = end_event_it != async_end_events_.rend();
      };
      auto emit_next_begin = [&begin_event_it, &has_begin_event, this]() {
        DoWriteEvent(*begin_event_it);
        begin_event_it++;
        has_begin_event = begin_event_it != async_begin_events_.end();
      };

      auto emit_next_instant_or_end = [&instant_event_it, &end_event_it,
                                       &emit_next_instant, &emit_next_end]() {
        if ((*instant_event_it)["ts"].asInt64() <=
            (*end_event_it)["ts"].asInt64()) {
          emit_next_instant();
        } else {
          emit_next_end();
        }
      };
      auto emit_next_instant_or_begin = [&instant_event_it, &begin_event_it,
                                         &emit_next_instant,
                                         &emit_next_begin]() {
        if ((*instant_event_it)["ts"].asInt64() <=
            (*begin_event_it)["ts"].asInt64()) {
          emit_next_instant();
        } else {
          emit_next_begin();
        }
      };
      auto emit_next_end_or_begin = [&end_event_it, &begin_event_it,
                                     &emit_next_end, &emit_next_begin]() {
        if ((*end_event_it)["ts"].asInt64() <=
            (*begin_event_it)["ts"].asInt64()) {
          emit_next_end();
        } else {
          emit_next_begin();
        }
      };

      // While we still have events in all iterators, consider each.
      while (has_instant_event && has_end_event && has_begin_event) {
        if ((*instant_event_it)["ts"].asInt64() <=
            (*end_event_it)["ts"].asInt64()) {
          emit_next_instant_or_begin();
        } else {
          emit_next_end_or_begin();
        }
      }

      // Only instant and end events left.
      while (has_instant_event && has_end_event) {
        emit_next_instant_or_end();
      }

      // Only instant and begin events left.
      while (has_instant_event && has_begin_event) {
        emit_next_instant_or_begin();
      }

      // Only end and begin events left.
      while (has_end_event && has_begin_event) {
        emit_next_end_or_begin();
      }

      // Remaining instant events.
      while (has_instant_event) {
        emit_next_instant();
      }

      // Remaining end events.
      while (has_end_event) {
        emit_next_end();
      }

      // Remaining begin events.
      while (has_begin_event) {
        emit_next_begin();
      }
    }

    void WriteMetadataEvent(const char* metadata_type,
                            const char* metadata_arg_name,
                            const char* metadata_arg_value,
                            uint32_t pid,
                            uint32_t tid) {
      if (label_filter_ && !label_filter_("traceEvents"))
        return;

      std::ostringstream ss;
      if (!first_event_)
        ss << ",\n";

      Json::Value value;
      value["ph"] = "M";
      value["cat"] = "__metadata";
      value["ts"] = 0;
      value["name"] = metadata_type;
      value["pid"] = Json::Int(pid);
      value["tid"] = Json::Int(tid);

      Json::Value args;
      args[metadata_arg_name] = metadata_arg_value;
      value["args"] = args;

      writer_->write(value, &ss);
      output_->AppendString(ss.str());
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
      metadata_["telemetry"][key] = static_cast<double>(value) / 1000.0;
    }

    void SetStats(const char* key, int64_t value) {
      metadata_["trace_processor_stats"][key] = Json::Int64(value);
    }

    void SetStats(const char* key, const IndexMap& indexed_values) {
      constexpr const char* kBufferStatsPrefix = "traced_buf_";

      // Stats for the same buffer should be grouped together in the JSON.
      if (strncmp(kBufferStatsPrefix, key, strlen(kBufferStatsPrefix)) == 0) {
        for (const auto& value : indexed_values) {
          metadata_["trace_processor_stats"]["traced_buf"][value.first]
                   [key + strlen(kBufferStatsPrefix)] =
                       Json::Int64(value.second);
        }
        return;
      }

      // Other indexed value stats are exported as array under their key.
      for (const auto& value : indexed_values) {
        metadata_["trace_processor_stats"][key][value.first] =
            Json::Int64(value.second);
      }
    }

    void AddSystemTraceData(const std::string& data) {
      system_trace_data_ += data;
    }

    void AddUserTraceData(const std::string& data) {
      if (user_trace_data_.empty())
        user_trace_data_ = "[";
      user_trace_data_ += data;
    }

   private:
    void WriteHeader() {
      if (!label_filter_)
        output_->AppendString("{\"traceEvents\":[\n");
    }

    void WriteFooter() {
      SortAndEmitAsyncEvents();

      // Filter metadata entries.
      if (metadata_filter_) {
        for (const auto& member : metadata_.getMemberNames()) {
          if (!metadata_filter_(member.c_str()))
            metadata_[member] = kStrippedArgument;
        }
      }

      if ((!label_filter_ || label_filter_("traceEvents")) &&
          !user_trace_data_.empty()) {
        user_trace_data_ += "]";

        Json::CharReaderBuilder builder;
        auto reader =
            std::unique_ptr<Json::CharReader>(builder.newCharReader());
        Json::Value result;
        if (reader->parse(user_trace_data_.data(),
                          user_trace_data_.data() + user_trace_data_.length(),
                          &result, nullptr)) {
          for (const auto& event : result) {
            WriteCommonEvent(event);
          }
        } else {
          PERFETTO_DLOG(
              "can't parse legacy user json trace export, skipping. data: %s",
              user_trace_data_.c_str());
        }
      }

      std::ostringstream ss;
      if (!label_filter_)
        ss << "]";

      if ((!label_filter_ || label_filter_("systemTraceEvents")) &&
          !system_trace_data_.empty()) {
        ss << ",\"systemTraceEvents\":\n";
        writer_->write(Json::Value(system_trace_data_), &ss);
      }

      if ((!label_filter_ || label_filter_("metadata")) && !metadata_.empty()) {
        ss << ",\"metadata\":\n";
        writer_->write(metadata_, &ss);
      }

      if (!label_filter_)
        ss << "}";

      output_->AppendString(ss.str());
    }

    void DoWriteEvent(const Json::Value& event) {
      std::ostringstream ss;
      if (!first_event_)
        ss << ",\n";

      ArgumentNameFilterPredicate argument_name_filter;
      bool strip_args =
          argument_filter_ &&
          !argument_filter_(event["cat"].asCString(), event["name"].asCString(),
                            &argument_name_filter);
      if ((strip_args || argument_name_filter) && event.isMember("args")) {
        Json::Value event_copy = event;
        if (strip_args) {
          event_copy["args"] = kStrippedArgument;
        } else {
          auto& args = event_copy["args"];
          for (const auto& member : event["args"].getMemberNames()) {
            if (!argument_name_filter(member.c_str()))
              args[member] = kStrippedArgument;
          }
        }
        writer_->write(event_copy, &ss);
      } else {
        writer_->write(event, &ss);
      }
      first_event_ = false;

      output_->AppendString(ss.str());
    }

    OutputWriter* output_;
    ArgumentFilterPredicate argument_filter_;
    MetadataFilterPredicate metadata_filter_;
    LabelFilterPredicate label_filter_;

    std::unique_ptr<Json::StreamWriter> writer_;
    bool first_event_;
    Json::Value metadata_;
    std::string system_trace_data_;
    std::string user_trace_data_;
    std::vector<Json::Value> async_begin_events_;
    std::vector<Json::Value> async_instant_events_;
    std::vector<Json::Value> async_end_events_;
  };

  class ArgsBuilder {
   public:
    explicit ArgsBuilder(const TraceStorage* storage)
        : storage_(storage),
          empty_value_(Json::objectValue),
          nan_value_(Json::StaticString("NaN")),
          inf_value_(Json::StaticString("Infinity")),
          neg_inf_value_(Json::StaticString("-Infinity")) {
      const auto& arg_table = storage_->arg_table();
      uint32_t count = arg_table.row_count();
      if (count == 0) {
        args_sets_.resize(1, empty_value_);
        return;
      }
      args_sets_.resize(arg_table.arg_set_id()[count - 1] + 1, empty_value_);

      for (uint32_t i = 0; i < count; ++i) {
        ArgSetId set_id = arg_table.arg_set_id()[i];
        const char* key = arg_table.key().GetString(i).c_str();
        Variadic value = storage_->GetArgValue(i);
        AppendArg(set_id, key, VariadicToJson(value));
      }
      PostprocessArgs();
    }

    const Json::Value& GetArgs(ArgSetId set_id) const {
      // If |set_id| was empty and added to the storage last, it may not be in
      // args_sets_.
      if (set_id > args_sets_.size())
        return empty_value_;
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
          if (std::isnan(variadic.real_value)) {
            return nan_value_;
          } else if (std::isinf(variadic.real_value) &&
                     variadic.real_value > 0) {
            return inf_value_;
          } else if (std::isinf(variadic.real_value) &&
                     variadic.real_value < 0) {
            return neg_inf_value_;
          } else {
            return variadic.real_value;
          }
        case Variadic::kPointer:
          return base::Uint64ToHexString(variadic.pointer_value);
        case Variadic::kBool:
          return variadic.bool_value;
        case Variadic::kNull:
          return base::Uint64ToHexString(0);
        case Variadic::kJson:
          Json::CharReaderBuilder b;
          auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());

          Json::Value result;
          std::string v = GetNonNullString(storage_, variadic.json_value);
          reader->parse(v.data(), v.data() + v.length(), &result, nullptr);
          return result;
      }
      PERFETTO_FATAL("Not reached");  // For gcc.
    }

    void AppendArg(ArgSetId set_id,
                   const std::string& key,
                   const Json::Value& value) {
      Json::Value* target = &args_sets_[set_id];
      for (base::StringSplitter parts(key, '.'); parts.Next();) {
        if (PERFETTO_UNLIKELY(!target->isNull() && !target->isObject())) {
          PERFETTO_DLOG("Malformed arguments. Can't append %s to %s.",
                        key.c_str(),
                        args_sets_[set_id].toStyledString().c_str());
          return;
        }
        std::string key_part = parts.cur_token();
        size_t bracketpos = key_part.find('[');
        if (bracketpos == key_part.npos) {  // A single item
          target = &(*target)[key_part];
        } else {  // A list item
          target = &(*target)[key_part.substr(0, bracketpos)];
          while (bracketpos != key_part.npos) {
            // We constructed this string from an int earlier in trace_processor
            // so it shouldn't be possible for this (or the StringToUInt32
            // below) to fail.
            std::string s =
                key_part.substr(bracketpos + 1, key_part.find(']', bracketpos) -
                                                    bracketpos - 1);
            if (PERFETTO_UNLIKELY(!target->isNull() && !target->isArray())) {
              PERFETTO_DLOG("Malformed arguments. Can't append %s to %s.",
                            key.c_str(),
                            args_sets_[set_id].toStyledString().c_str());
              return;
            }
            base::Optional<uint32_t> index = base::StringToUInt32(s);
            if (PERFETTO_UNLIKELY(!index)) {
              PERFETTO_ELOG("Expected to be able to extract index from %s",
                            key_part.c_str());
              return;
            }
            target = &(*target)[index.value()];
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
          if (args["task"].empty())
            args.removeMember("task");
        }
        if (args.isMember("source")) {
          Json::Value source = args["source"];
          if (source.isObject() && source.isMember("function_name")) {
            args["function_name"] = source["function_name"];
            args["file_name"] = source["file_name"];
            args.removeMember("source");
          }
        }
      }
    }

    const TraceStorage* storage_;
    std::vector<Json::Value> args_sets_;
    const Json::Value empty_value_;
    const Json::Value nan_value_;
    const Json::Value inf_value_;
    const Json::Value neg_inf_value_;
  };

  util::Status MapUniquePidsAndTids() {
    const auto& process_table = storage_->process_table();
    for (UniquePid upid = 0; upid < process_table.row_count(); upid++) {
      uint32_t exported_pid = process_table.pid()[upid];
      auto it_and_inserted =
          exported_pids_to_upids_.emplace(exported_pid, upid);
      if (!it_and_inserted.second) {
        exported_pid = NextExportedPidOrTidForDuplicates();
        it_and_inserted = exported_pids_to_upids_.emplace(exported_pid, upid);
      }
      upids_to_exported_pids_.emplace(upid, exported_pid);
    }

    const auto& thread_table = storage_->thread_table();
    for (UniqueTid utid = 0; utid < thread_table.row_count(); utid++) {
      uint32_t exported_pid = 0;
      base::Optional<UniquePid> upid = thread_table.upid()[utid];
      if (upid) {
        auto exported_pid_it = upids_to_exported_pids_.find(*upid);
        PERFETTO_DCHECK(exported_pid_it != upids_to_exported_pids_.end());
        exported_pid = exported_pid_it->second;
      }

      uint32_t exported_tid = thread_table.tid()[utid];
      auto it_and_inserted = exported_pids_and_tids_to_utids_.emplace(
          std::make_pair(exported_pid, exported_tid), utid);
      if (!it_and_inserted.second) {
        exported_tid = NextExportedPidOrTidForDuplicates();
        it_and_inserted = exported_pids_and_tids_to_utids_.emplace(
            std::make_pair(exported_pid, exported_tid), utid);
      }
      utids_to_exported_pids_and_tids_.emplace(
          utid, std::make_pair(exported_pid, exported_tid));
    }

    return util::OkStatus();
  }

  util::Status ExportThreadNames() {
    const auto& thread_table = storage_->thread_table();
    for (UniqueTid utid = 0; utid < thread_table.row_count(); ++utid) {
      auto opt_name = thread_table.name()[utid];
      if (opt_name.has_value()) {
        const char* thread_name = GetNonNullString(storage_, opt_name);
        auto pid_and_tid = UtidToPidAndTid(utid);
        writer_.WriteMetadataEvent("thread_name", "name", thread_name,
                                   pid_and_tid.first, pid_and_tid.second);
      }
    }
    return util::OkStatus();
  }

  util::Status ExportProcessNames() {
    const auto& process_table = storage_->process_table();
    for (UniquePid upid = 0; upid < process_table.row_count(); ++upid) {
      auto opt_name = process_table.name()[upid];
      if (opt_name.has_value()) {
        const char* process_name = GetNonNullString(storage_, opt_name);
        writer_.WriteMetadataEvent("process_name", "name", process_name,
                                   UpidToPid(upid), /*tid=*/0);
      }
    }
    return util::OkStatus();
  }

  // For each process it writes an approximate uptime, based on the process'
  // start time and the last slice in the entire trace. This same last slice is
  // used with all processes, so the process could have ended earlier.
  util::Status ExportProcessUptimes() {
    int64_t last_timestamp_ns = FindLastSliceTimestamp();
    if (last_timestamp_ns <= 0)
      return util::OkStatus();

    const auto& process_table = storage_->process_table();
    for (UniquePid upid = 0; upid < process_table.row_count(); ++upid) {
      base::Optional<int64_t> start_timestamp_ns =
          process_table.start_ts()[upid];
      if (!start_timestamp_ns.has_value())
        continue;

      int64_t process_uptime_seconds =
          (last_timestamp_ns - start_timestamp_ns.value()) /
          (1000 * 1000 * 1000);

      writer_.WriteMetadataEvent("process_uptime_seconds", "uptime",
                                 std::to_string(process_uptime_seconds).c_str(),
                                 UpidToPid(upid), /*tid=*/0);
    }

    return util::OkStatus();
  }

  // Returns the last slice's end timestamp for the entire trace. If no slices
  // are found 0 is returned.
  int64_t FindLastSliceTimestamp() {
    int64_t last_ts = 0;
    const auto& slices = storage_->slice_table();
    for (uint32_t i = 0; i < slices.row_count(); ++i) {
      int64_t duration_ns = slices.dur()[i];
      int64_t timestamp_ns = slices.ts()[i];

      if (duration_ns + timestamp_ns > last_ts) {
        last_ts = duration_ns + timestamp_ns;
      }
    }
    return last_ts;
  }

  util::Status ExportSlices() {
    const auto& slices = storage_->slice_table();
    for (auto it = slices.IterateRows(); it; ++it) {
      // Skip slices with empty category - these are ftrace/system slices that
      // were also imported into the raw table and will be exported from there
      // by trace_to_text.
      // TODO(b/153609716): Add a src column or do_not_export flag instead.
      if (!it.category())
        continue;
      auto cat = storage_->GetString(*it.category());
      if (cat.c_str() == nullptr || cat == "binder")
        continue;

      Json::Value event;
      event["ts"] = Json::Int64(it.ts() / 1000);
      event["cat"] = GetNonNullString(storage_, it.category());
      event["name"] = GetNonNullString(storage_, it.name());
      event["pid"] = 0;
      event["tid"] = 0;

      base::Optional<UniqueTid> legacy_utid;
      std::string legacy_phase;

      event["args"] = args_builder_.GetArgs(it.arg_set_id());  // Makes a copy.
      if (event["args"].isMember(kLegacyEventArgsKey)) {
        const auto& legacy_args = event["args"][kLegacyEventArgsKey];

        if (legacy_args.isMember(kLegacyEventPassthroughUtidKey)) {
          legacy_utid = legacy_args[kLegacyEventPassthroughUtidKey].asUInt();
        }
        if (legacy_args.isMember(kLegacyEventPhaseKey)) {
          legacy_phase = legacy_args[kLegacyEventPhaseKey].asString();
        }

        event["args"].removeMember(kLegacyEventArgsKey);
      }

      // To prevent duplicate export of slices, only export slices on descriptor
      // or chrome tracks (i.e. TrackEvent slices). Slices on other tracks may
      // also be present as raw events and handled by trace_to_text. Only add
      // more track types here if they are not already covered by trace_to_text.
      TrackId track_id = it.track_id();

      const auto& track_table = storage_->track_table();

      auto track_row_ref = *track_table.FindById(track_id);
      auto track_args_id = track_row_ref.source_arg_set_id();
      const Json::Value* track_args = nullptr;
      bool legacy_chrome_track = false;
      bool is_child_track = false;
      if (track_args_id) {
        track_args = &args_builder_.GetArgs(*track_args_id);
        legacy_chrome_track = (*track_args)["source"].asString() == "chrome";
        is_child_track = track_args->isMember("is_root_in_scope") &&
                         !(*track_args)["is_root_in_scope"].asBool();
      }

      const auto& thread_track = storage_->thread_track_table();
      const auto& process_track = storage_->process_track_table();
      const auto& virtual_track_slices = storage_->virtual_track_slices();

      int64_t duration_ns = it.dur();
      base::Optional<int64_t> thread_ts_ns;
      base::Optional<int64_t> thread_duration_ns;
      base::Optional<int64_t> thread_instruction_count;
      base::Optional<int64_t> thread_instruction_delta;

      if (it.thread_dur()) {
        thread_ts_ns = it.thread_ts();
        thread_duration_ns = it.thread_dur();
        thread_instruction_count = it.thread_instruction_count();
        thread_instruction_delta = it.thread_instruction_delta();
      } else {
        SliceId id = it.id();
        base::Optional<uint32_t> vtrack_slice_row =
            virtual_track_slices.FindRowForSliceId(id);
        if (vtrack_slice_row) {
          thread_ts_ns =
              virtual_track_slices.thread_timestamp_ns()[*vtrack_slice_row];
          thread_duration_ns =
              virtual_track_slices.thread_duration_ns()[*vtrack_slice_row];
          thread_instruction_count =
              virtual_track_slices
                  .thread_instruction_counts()[*vtrack_slice_row];
          thread_instruction_delta =
              virtual_track_slices
                  .thread_instruction_deltas()[*vtrack_slice_row];
        }
      }

      auto opt_thread_track_row = thread_track.id().IndexOf(TrackId{track_id});

      if (opt_thread_track_row && !is_child_track) {
        // Synchronous (thread) slice or instant event.
        UniqueTid utid = thread_track.utid()[*opt_thread_track_row];
        auto pid_and_tid = UtidToPidAndTid(utid);
        event["pid"] = Json::Int(pid_and_tid.first);
        event["tid"] = Json::Int(pid_and_tid.second);

        if (duration_ns == 0) {
          if (legacy_phase.empty()) {
            // Use "I" instead of "i" phase for backwards-compat with old
            // consumers.
            event["ph"] = "I";
          } else {
            event["ph"] = legacy_phase;
          }
          if (thread_ts_ns && thread_ts_ns > 0) {
            event["tts"] = Json::Int64(*thread_ts_ns / 1000);
          }
          if (thread_instruction_count && *thread_instruction_count > 0) {
            event["ticount"] = Json::Int64(*thread_instruction_count);
          }
          event["s"] = "t";
        } else {
          if (duration_ns > 0) {
            event["ph"] = "X";
            event["dur"] = Json::Int64(duration_ns / 1000);
          } else {
            // If the slice didn't finish, the duration may be negative. Only
            // write a begin event without end event in this case.
            event["ph"] = "B";
          }
          if (thread_ts_ns && *thread_ts_ns > 0) {
            event["tts"] = Json::Int64(*thread_ts_ns / 1000);
            // Only write thread duration for completed events.
            if (duration_ns > 0 && thread_duration_ns)
              event["tdur"] = Json::Int64(*thread_duration_ns / 1000);
          }
          if (thread_instruction_count && *thread_instruction_count > 0) {
            event["ticount"] = Json::Int64(*thread_instruction_count);
            // Only write thread instruction delta for completed events.
            if (duration_ns > 0 && thread_instruction_delta)
              event["tidelta"] = Json::Int64(*thread_instruction_delta);
          }
        }
        writer_.WriteCommonEvent(event);
      } else if (is_child_track ||
                 (legacy_chrome_track && track_args->isMember("source_id"))) {
        // Async event slice.
        auto opt_process_row = process_track.id().IndexOf(TrackId{track_id});
        if (legacy_chrome_track) {
          // Legacy async tracks are always process-associated and have args.
          PERFETTO_DCHECK(opt_process_row);
          PERFETTO_DCHECK(track_args);
          uint32_t upid = process_track.upid()[*opt_process_row];
          uint32_t exported_pid = UpidToPid(upid);
          event["pid"] = Json::Int(exported_pid);
          event["tid"] =
              Json::Int(legacy_utid ? UtidToPidAndTid(*legacy_utid).second
                                    : exported_pid);

          // Preserve original event IDs for legacy tracks. This is so that e.g.
          // memory dump IDs show up correctly in the JSON trace.
          PERFETTO_DCHECK(track_args->isMember("source_id"));
          PERFETTO_DCHECK(track_args->isMember("source_id_is_process_scoped"));
          PERFETTO_DCHECK(track_args->isMember("source_scope"));
          uint64_t source_id =
              static_cast<uint64_t>((*track_args)["source_id"].asInt64());
          std::string source_scope = (*track_args)["source_scope"].asString();
          if (!source_scope.empty())
            event["scope"] = source_scope;
          bool source_id_is_process_scoped =
              (*track_args)["source_id_is_process_scoped"].asBool();
          if (source_id_is_process_scoped) {
            event["id2"]["local"] = base::Uint64ToHexString(source_id);
          } else {
            // Some legacy importers don't understand "id2" fields, so we use
            // the "usually" global "id" field instead. This works as long as
            // the event phase is not in {'N', 'D', 'O', '(', ')'}, see
            // "LOCAL_ID_PHASES" in catapult.
            event["id"] = base::Uint64ToHexString(source_id);
          }
        } else {
          if (opt_thread_track_row) {
            UniqueTid utid = thread_track.utid()[*opt_thread_track_row];
            auto pid_and_tid = UtidToPidAndTid(utid);
            event["pid"] = Json::Int(pid_and_tid.first);
            event["tid"] = Json::Int(pid_and_tid.second);
            event["id2"]["local"] = base::Uint64ToHexString(track_id.value);
          } else if (opt_process_row) {
            uint32_t upid = process_track.upid()[*opt_process_row];
            uint32_t exported_pid = UpidToPid(upid);
            event["pid"] = Json::Int(exported_pid);
            event["tid"] =
                Json::Int(legacy_utid ? UtidToPidAndTid(*legacy_utid).second
                                      : exported_pid);
            event["id2"]["local"] = base::Uint64ToHexString(track_id.value);
          } else {
            if (legacy_utid) {
              auto pid_and_tid = UtidToPidAndTid(*legacy_utid);
              event["pid"] = Json::Int(pid_and_tid.first);
              event["tid"] = Json::Int(pid_and_tid.second);
            }

            // Some legacy importers don't understand "id2" fields, so we use
            // the "usually" global "id" field instead. This works as long as
            // the event phase is not in {'N', 'D', 'O', '(', ')'}, see
            // "LOCAL_ID_PHASES" in catapult.
            event["id"] = base::Uint64ToHexString(track_id.value);
          }
        }

        if (thread_ts_ns && *thread_ts_ns > 0) {
          event["tts"] = Json::Int64(*thread_ts_ns / 1000);
          event["use_async_tts"] = Json::Int(1);
        }
        if (thread_instruction_count && *thread_instruction_count > 0) {
          event["ticount"] = Json::Int64(*thread_instruction_count);
          event["use_async_tts"] = Json::Int(1);
        }

        if (duration_ns == 0) {
          if (legacy_phase.empty()) {
            // Instant async event.
            event["ph"] = "n";
            writer_.AddAsyncInstantEvent(event);
          } else {
            // Async step events.
            event["ph"] = legacy_phase;
            writer_.AddAsyncBeginEvent(event);
          }
        } else {  // Async start and end.
          event["ph"] = legacy_phase.empty() ? "b" : legacy_phase;
          writer_.AddAsyncBeginEvent(event);
          // If the slice didn't finish, the duration may be negative. Don't
          // write the end event in this case.
          if (duration_ns > 0) {
            event["ph"] = legacy_phase.empty() ? "e" : "F";
            event["ts"] = Json::Int64((it.ts() + duration_ns) / 1000);
            if (thread_ts_ns && thread_duration_ns && *thread_ts_ns > 0) {
              event["tts"] =
                  Json::Int64((*thread_ts_ns + *thread_duration_ns) / 1000);
            }
            if (thread_instruction_count && thread_instruction_delta &&
                *thread_instruction_count > 0) {
              event["ticount"] = Json::Int64(
                  (*thread_instruction_count + *thread_instruction_delta));
            }
            event["args"].clear();
            writer_.AddAsyncEndEvent(event);
          }
        }
      } else {
        // Global or process-scoped instant event.
        PERFETTO_DCHECK(legacy_chrome_track || !is_child_track);
        if (duration_ns != 0) {
          // We don't support exporting slices on the default global or process
          // track to JSON (JSON only supports instant events on these tracks).
          PERFETTO_DLOG(
              "skipping non-instant slice on global or process track");
        } else {
          if (legacy_phase.empty()) {
            // Use "I" instead of "i" phase for backwards-compat with old
            // consumers.
            event["ph"] = "I";
          } else {
            event["ph"] = legacy_phase;
          }

          auto opt_process_row = process_track.id().IndexOf(TrackId{track_id});
          if (opt_process_row.has_value()) {
            uint32_t upid = process_track.upid()[*opt_process_row];
            uint32_t exported_pid = UpidToPid(upid);
            event["pid"] = Json::Int(exported_pid);
            event["tid"] =
                Json::Int(legacy_utid ? UtidToPidAndTid(*legacy_utid).second
                                      : exported_pid);
            event["s"] = "p";
          } else {
            event["s"] = "g";
          }
          writer_.WriteCommonEvent(event);
        }
      }
    }
    return util::OkStatus();
  }

  base::Optional<Json::Value> CreateFlowEventV1(uint32_t flow_id,
                                                SliceId slice_id,
                                                std::string name,
                                                std::string cat,
                                                Json::Value args,
                                                bool flow_begin) {
    const auto& slices = storage_->slice_table();
    const auto& thread_tracks = storage_->thread_track_table();

    auto opt_slice_idx = slices.id().IndexOf(slice_id);
    if (!opt_slice_idx)
      return base::nullopt;
    uint32_t slice_idx = opt_slice_idx.value();

    TrackId track_id = storage_->slice_table().track_id()[slice_idx];
    auto opt_thread_track_idx = thread_tracks.id().IndexOf(track_id);
    // catapult only supports flow events attached to thread-track slices
    if (!opt_thread_track_idx)
      return base::nullopt;

    UniqueTid utid = thread_tracks.utid()[opt_thread_track_idx.value()];
    auto pid_and_tid = UtidToPidAndTid(utid);
    Json::Value event;
    event["id"] = flow_id;
    event["pid"] = Json::Int(pid_and_tid.first);
    event["tid"] = Json::Int(pid_and_tid.second);
    event["cat"] = cat;
    event["name"] = name;
    event["ph"] = (flow_begin ? "s" : "f");
    event["ts"] = Json::Int64(slices.ts()[slice_idx] / 1000);
    if (!flow_begin) {
      event["bp"] = "e";
    }
    event["args"] = std::move(args);
    return std::move(event);
  }

  util::Status ExportFlows() {
    const auto& flow_table = storage_->flow_table();
    const auto& slice_table = storage_->slice_table();
    for (uint32_t i = 0; i < flow_table.row_count(); i++) {
      SliceId slice_out = flow_table.slice_out()[i];
      SliceId slice_in = flow_table.slice_in()[i];
      uint32_t arg_set_id = flow_table.arg_set_id()[i];

      std::string cat;
      std::string name;
      auto args = args_builder_.GetArgs(arg_set_id);
      if (arg_set_id != kInvalidArgSetId) {
        cat = args["cat"].asString();
        name = args["name"].asString();
        // Don't export these args since they are only used for this export and
        // weren't part of the original event.
        args.removeMember("name");
        args.removeMember("cat");
      } else {
        auto opt_slice_out_idx = slice_table.id().IndexOf(slice_out);
        PERFETTO_DCHECK(opt_slice_out_idx.has_value());
        base::Optional<StringId> cat_id =
            slice_table.category()[opt_slice_out_idx.value()];
        base::Optional<StringId> name_id =
            slice_table.name()[opt_slice_out_idx.value()];
        cat = GetNonNullString(storage_, cat_id);
        name = GetNonNullString(storage_, name_id);
      }

      auto out_event = CreateFlowEventV1(i, slice_out, name, cat, args,
                                         /* flow_begin = */ true);
      auto in_event = CreateFlowEventV1(i, slice_in, name, cat, std::move(args),
                                        /* flow_begin = */ false);

      if (out_event && in_event) {
        writer_.WriteCommonEvent(out_event.value());
        writer_.WriteCommonEvent(in_event.value());
      }
    }
    return util::OkStatus();
  }

  Json::Value ConvertLegacyRawEventToJson(uint32_t index) {
    const auto& events = storage_->raw_table();

    Json::Value event;
    event["ts"] = Json::Int64(events.ts()[index] / 1000);

    UniqueTid utid = static_cast<UniqueTid>(events.utid()[index]);
    auto pid_and_tid = UtidToPidAndTid(utid);
    event["pid"] = Json::Int(pid_and_tid.first);
    event["tid"] = Json::Int(pid_and_tid.second);

    // Raw legacy events store all other params in the arg set. Make a copy of
    // the converted args here, parse, and then remove the legacy params.
    event["args"] = args_builder_.GetArgs(events.arg_set_id()[index]);
    const Json::Value& legacy_args = event["args"][kLegacyEventArgsKey];

    PERFETTO_DCHECK(legacy_args.isMember(kLegacyEventCategoryKey));
    event["cat"] = legacy_args[kLegacyEventCategoryKey];

    PERFETTO_DCHECK(legacy_args.isMember(kLegacyEventNameKey));
    event["name"] = legacy_args[kLegacyEventNameKey];

    PERFETTO_DCHECK(legacy_args.isMember(kLegacyEventPhaseKey));
    event["ph"] = legacy_args[kLegacyEventPhaseKey];

    // Object snapshot events are supposed to have a mandatory "snapshot" arg,
    // which may be removed in trace processor if it is empty.
    if (legacy_args[kLegacyEventPhaseKey] == "O" &&
        !event["args"].isMember("snapshot")) {
      event["args"]["snapshot"] = Json::Value(Json::objectValue);
    }

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
      event["id"] = base::Uint64ToHexString(
          legacy_args[kLegacyEventUnscopedIdKey].asUInt64());
    }

    if (legacy_args.isMember(kLegacyEventGlobalIdKey)) {
      event["id2"]["global"] = base::Uint64ToHexString(
          legacy_args[kLegacyEventGlobalIdKey].asUInt64());
    }

    if (legacy_args.isMember(kLegacyEventLocalIdKey)) {
      event["id2"]["local"] = base::Uint64ToHexString(
          legacy_args[kLegacyEventLocalIdKey].asUInt64());
    }

    if (legacy_args.isMember(kLegacyEventIdScopeKey))
      event["scope"] = legacy_args[kLegacyEventIdScopeKey];

    event["args"].removeMember(kLegacyEventArgsKey);

    return event;
  }

  util::Status ExportRawEvents() {
    base::Optional<StringId> raw_legacy_event_key_id =
        storage_->string_pool().GetId("track_event.legacy_event");
    base::Optional<StringId> raw_legacy_system_trace_event_id =
        storage_->string_pool().GetId("chrome_event.legacy_system_trace");
    base::Optional<StringId> raw_legacy_user_trace_event_id =
        storage_->string_pool().GetId("chrome_event.legacy_user_trace");
    base::Optional<StringId> raw_chrome_metadata_event_id =
        storage_->string_pool().GetId("chrome_event.metadata");

    const auto& events = storage_->raw_table();
    for (uint32_t i = 0; i < events.row_count(); ++i) {
      if (raw_legacy_event_key_id &&
          events.name()[i] == *raw_legacy_event_key_id) {
        Json::Value event = ConvertLegacyRawEventToJson(i);
        writer_.WriteCommonEvent(event);
      } else if (raw_legacy_system_trace_event_id &&
                 events.name()[i] == *raw_legacy_system_trace_event_id) {
        Json::Value args = args_builder_.GetArgs(events.arg_set_id()[i]);
        PERFETTO_DCHECK(args.isMember("data"));
        writer_.AddSystemTraceData(args["data"].asString());
      } else if (raw_legacy_user_trace_event_id &&
                 events.name()[i] == *raw_legacy_user_trace_event_id) {
        Json::Value args = args_builder_.GetArgs(events.arg_set_id()[i]);
        PERFETTO_DCHECK(args.isMember("data"));
        writer_.AddUserTraceData(args["data"].asString());
      } else if (raw_chrome_metadata_event_id &&
                 events.name()[i] == *raw_chrome_metadata_event_id) {
        Json::Value args = args_builder_.GetArgs(events.arg_set_id()[i]);
        writer_.MergeMetadata(args);
      }
    }
    return util::OkStatus();
  }

  class MergedProfileSamplesEmitter {
   public:
    // The TraceFormatWriter must outlive this instance.
    MergedProfileSamplesEmitter(TraceFormatWriter& writer) : writer_(writer) {}

    uint64_t AddEventForUtid(UniqueTid utid,
                             int64_t ts,
                             CallsiteId callsite_id,
                             const Json::Value& event) {
      auto current_sample = current_events_.find(utid);

      // If there's a current entry for our thread and it matches the callsite
      // of the new sample, update the entry with the new timestamp. Otherwise
      // create a new entry.
      if (current_sample != current_events_.end() &&
          current_sample->second.callsite_id() == callsite_id) {
        current_sample->second.UpdateWithNewSample(ts);
        return current_sample->second.event_id();
      } else {
        if (current_sample != current_events_.end())
          current_events_.erase(current_sample);

        auto new_entry = current_events_.emplace(
            std::piecewise_construct, std::forward_as_tuple(utid),
            std::forward_as_tuple(writer_, callsite_id, ts, event));
        return new_entry.first->second.event_id();
      }
    }

    static uint64_t GenerateNewEventId() {
      // "n"-phase events are nestable async events which get tied together
      // with their id, so we need to give each one a unique ID as we only
      // want the samples to show up on their own track in the trace-viewer
      // but not nested together (unless they're nested under a merged event).
      static size_t g_id_counter = 0;
      return ++g_id_counter;
    }

   private:
    class Sample {
     public:
      Sample(TraceFormatWriter& writer,
             CallsiteId callsite_id,
             int64_t ts,
             const Json::Value& event)
          : writer_(writer),
            callsite_id_(callsite_id),
            begin_ts_(ts),
            end_ts_(ts),
            event_(event),
            event_id_(MergedProfileSamplesEmitter::GenerateNewEventId()),
            sample_count_(1) {}

      ~Sample() {
        // No point writing a merged event if we only got a single sample
        // as ExportCpuProfileSamples will already be writing the instant event.
        if (sample_count_ == 1)
          return;

        event_["id"] = base::Uint64ToHexString(event_id_);

        // Write the BEGIN event.
        event_["ph"] = "b";
        // We subtract 1us as a workaround for the first async event not
        // nesting underneath the parent event if the timestamp is identical.
        int64_t begin_in_us_ = begin_ts_ / 1000;
        event_["ts"] = Json::Int64(std::min(begin_in_us_ - 1, begin_in_us_));
        writer_.WriteCommonEvent(event_);

        // Write the END event.
        event_["ph"] = "e";
        event_["ts"] = Json::Int64(end_ts_ / 1000);
        // No need for args for the end event; remove them to save some space.
        event_["args"].clear();
        writer_.WriteCommonEvent(event_);
      }

      void UpdateWithNewSample(int64_t ts) {
        // We assume samples for a given thread will appear in timestamp
        // order; if this assumption stops holding true, we'll have to sort the
        // samples first.
        if (ts < end_ts_ || begin_ts_ > ts) {
          PERFETTO_ELOG(
              "Got an timestamp out of sequence while merging stack samples "
              "during JSON export!\n");
          PERFETTO_DCHECK(false);
        }

        end_ts_ = ts;
        sample_count_++;
      }

      uint64_t event_id() const { return event_id_; }
      CallsiteId callsite_id() const { return callsite_id_; }

     public:
      Sample(const Sample&) = delete;
      Sample& operator=(const Sample&) = delete;
      Sample& operator=(Sample&& value) = delete;

      TraceFormatWriter& writer_;
      CallsiteId callsite_id_;
      int64_t begin_ts_;
      int64_t end_ts_;
      Json::Value event_;
      uint64_t event_id_;
      size_t sample_count_;
    };

    MergedProfileSamplesEmitter(const MergedProfileSamplesEmitter&) = delete;
    MergedProfileSamplesEmitter& operator=(const MergedProfileSamplesEmitter&) =
        delete;
    MergedProfileSamplesEmitter& operator=(
        MergedProfileSamplesEmitter&& value) = delete;

    std::unordered_map<UniqueTid, Sample> current_events_;
    TraceFormatWriter& writer_;
  };

  util::Status ExportCpuProfileSamples() {
    MergedProfileSamplesEmitter merged_sample_emitter(writer_);

    const tables::CpuProfileStackSampleTable& samples =
        storage_->cpu_profile_stack_sample_table();
    for (uint32_t i = 0; i < samples.row_count(); ++i) {
      Json::Value event;
      event["ts"] = Json::Int64(samples.ts()[i] / 1000);

      UniqueTid utid = static_cast<UniqueTid>(samples.utid()[i]);
      auto pid_and_tid = UtidToPidAndTid(utid);
      event["pid"] = Json::Int(pid_and_tid.first);
      event["tid"] = Json::Int(pid_and_tid.second);

      event["ph"] = "n";
      event["cat"] = "disabled-by-default-cpu_profiler";
      event["name"] = "StackCpuSampling";
      event["s"] = "t";

      // Add a dummy thread timestamp to this event to match the format of
      // instant events. Useful in the UI to view args of a selected group of
      // samples.
      event["tts"] = Json::Int64(1);

      const auto& callsites = storage_->stack_profile_callsite_table();
      const auto& frames = storage_->stack_profile_frame_table();
      const auto& mappings = storage_->stack_profile_mapping_table();

      std::vector<std::string> callstack;
      base::Optional<CallsiteId> opt_callsite_id = samples.callsite_id()[i];

      while (opt_callsite_id) {
        CallsiteId callsite_id = *opt_callsite_id;
        uint32_t callsite_row = *callsites.id().IndexOf(callsite_id);

        FrameId frame_id = callsites.frame_id()[callsite_row];
        uint32_t frame_row = *frames.id().IndexOf(frame_id);

        MappingId mapping_id = frames.mapping()[frame_row];
        uint32_t mapping_row = *mappings.id().IndexOf(mapping_id);

        NullTermStringView symbol_name;
        auto opt_symbol_set_id = frames.symbol_set_id()[frame_row];
        if (opt_symbol_set_id) {
          symbol_name = storage_->GetString(
              storage_->symbol_table().name()[*opt_symbol_set_id]);
        }

        base::StackString<1024> frame_entry(
            "%s - %s [%s]\n",
            (symbol_name.empty()
                 ? base::Uint64ToHexString(
                       static_cast<uint64_t>(frames.rel_pc()[frame_row]))
                       .c_str()
                 : symbol_name.c_str()),
            GetNonNullString(storage_, mappings.name()[mapping_row]),
            GetNonNullString(storage_, mappings.build_id()[mapping_row]));

        callstack.emplace_back(frame_entry.ToStdString());

        opt_callsite_id = callsites.parent_id()[callsite_row];
      }

      std::string merged_callstack;
      for (auto entry = callstack.rbegin(); entry != callstack.rend();
           ++entry) {
        merged_callstack += *entry;
      }

      event["args"]["frames"] = merged_callstack;
      event["args"]["process_priority"] = samples.process_priority()[i];

      // TODO(oysteine): Used for backwards compatibility with the memlog
      // pipeline, should remove once we've switched to looking directly at the
      // tid.
      event["args"]["thread_id"] = Json::Int(pid_and_tid.second);

      // Emit duration events for adjacent samples with the same callsite.
      // For now, only do this when the trace has already been symbolized i.e.
      // are not directly output by Chrome, to avoid interfering with other
      // processing pipelines.
      base::Optional<CallsiteId> opt_current_callsite_id =
          samples.callsite_id()[i];

      if (opt_current_callsite_id && storage_->symbol_table().row_count() > 0) {
        uint64_t parent_event_id = merged_sample_emitter.AddEventForUtid(
            utid, samples.ts()[i], *opt_current_callsite_id, event);
        event["id"] = base::Uint64ToHexString(parent_event_id);
      } else {
        event["id"] = base::Uint64ToHexString(
            MergedProfileSamplesEmitter::GenerateNewEventId());
      }

      writer_.WriteCommonEvent(event);
    }

    return util::OkStatus();
  }

  util::Status ExportMetadata() {
    const auto& trace_metadata = storage_->metadata_table();
    const auto& keys = trace_metadata.name();
    const auto& int_values = trace_metadata.int_value();
    const auto& str_values = trace_metadata.str_value();

    // Create a mapping from key string ids to keys.
    std::unordered_map<StringId, metadata::KeyId> key_map;
    for (uint32_t i = 0; i < metadata::kNumKeys; ++i) {
      auto id = *storage_->string_pool().GetId(metadata::kNames[i]);
      key_map[id] = static_cast<metadata::KeyId>(i);
    }

    for (uint32_t pos = 0; pos < trace_metadata.row_count(); pos++) {
      auto key_it = key_map.find(keys[pos]);
      // Skip exporting dynamic entries; the cr-xxx entries that come from
      // the ChromeMetadata proto message are already exported from the raw
      // table.
      if (key_it == key_map.end())
        continue;

      // Cast away from enum type, as otherwise -Wswitch-enum will demand an
      // exhaustive list of cases, even if there's a default case.
      metadata::KeyId key = key_it->second;
      switch (static_cast<size_t>(key)) {
        case metadata::benchmark_description:
          writer_.AppendTelemetryMetadataString(
              "benchmarkDescriptions", str_values.GetString(pos).c_str());
          break;

        case metadata::benchmark_name:
          writer_.AppendTelemetryMetadataString(
              "benchmarks", str_values.GetString(pos).c_str());
          break;

        case metadata::benchmark_start_time_us:
          writer_.SetTelemetryMetadataTimestamp("benchmarkStart",
                                                *int_values[pos]);
          break;

        case metadata::benchmark_had_failures:
          writer_.AppendTelemetryMetadataBool("hadFailures", *int_values[pos]);
          break;

        case metadata::benchmark_label:
          writer_.AppendTelemetryMetadataString(
              "labels", str_values.GetString(pos).c_str());
          break;

        case metadata::benchmark_story_name:
          writer_.AppendTelemetryMetadataString(
              "stories", str_values.GetString(pos).c_str());
          break;

        case metadata::benchmark_story_run_index:
          writer_.AppendTelemetryMetadataInt("storysetRepeats",
                                             *int_values[pos]);
          break;

        case metadata::benchmark_story_run_time_us:
          writer_.SetTelemetryMetadataTimestamp("traceStart", *int_values[pos]);
          break;

        case metadata::benchmark_story_tags:  // repeated
          writer_.AppendTelemetryMetadataString(
              "storyTags", str_values.GetString(pos).c_str());
          break;

        default:
          PERFETTO_DLOG("Ignoring metadata key %zu", static_cast<size_t>(key));
          break;
      }
    }
    return util::OkStatus();
  }

  util::Status ExportStats() {
    const auto& stats = storage_->stats();

    for (size_t idx = 0; idx < stats::kNumKeys; idx++) {
      if (stats::kTypes[idx] == stats::kSingle) {
        writer_.SetStats(stats::kNames[idx], stats[idx].value);
      } else {
        PERFETTO_DCHECK(stats::kTypes[idx] == stats::kIndexed);
        writer_.SetStats(stats::kNames[idx], stats[idx].indexed_values);
      }
    }

    return util::OkStatus();
  }

  util::Status ExportMemorySnapshots() {
    const auto& memory_snapshots = storage_->memory_snapshot_table();
    base::Optional<StringId> private_footprint_id =
        storage_->string_pool().GetId("chrome.private_footprint_kb");
    base::Optional<StringId> peak_resident_set_id =
        storage_->string_pool().GetId("chrome.peak_resident_set_kb");

    for (uint32_t memory_index = 0; memory_index < memory_snapshots.row_count();
         ++memory_index) {
      Json::Value event_base;

      event_base["ph"] = "v";
      event_base["cat"] = "disabled-by-default-memory-infra";
      auto snapshot_id = memory_snapshots.id()[memory_index].value;
      event_base["id"] = base::Uint64ToHexString(snapshot_id);
      int64_t snapshot_ts = memory_snapshots.timestamp()[memory_index];
      event_base["ts"] = Json::Int64(snapshot_ts / 1000);
      // TODO(crbug:1116359): Add dump type to the snapshot proto
      // to properly fill event_base["name"]
      event_base["name"] = "periodic_interval";
      event_base["args"]["dumps"]["level_of_detail"] = GetNonNullString(
          storage_, memory_snapshots.detail_level()[memory_index]);

      // Export OS dump events for processes with relevant data.
      const auto& process_table = storage_->process_table();
      for (UniquePid upid = 0; upid < process_table.row_count(); ++upid) {
        Json::Value event =
            FillInProcessEventDetails(event_base, process_table.pid()[upid]);
        Json::Value& totals = event["args"]["dumps"]["process_totals"];

        const auto& process_counters = storage_->process_counter_track_table();

        for (uint32_t counter_index = 0;
             counter_index < process_counters.row_count(); ++counter_index) {
          if (process_counters.upid()[counter_index] != upid)
            continue;
          TrackId track_id = process_counters.id()[counter_index];
          if (private_footprint_id && (process_counters.name()[counter_index] ==
                                       private_footprint_id)) {
            totals["private_footprint_bytes"] = base::Uint64ToHexStringNoPrefix(
                GetCounterValue(track_id, snapshot_ts));
          } else if (peak_resident_set_id &&
                     (process_counters.name()[counter_index] ==
                      peak_resident_set_id)) {
            totals["peak_resident_set_size"] = base::Uint64ToHexStringNoPrefix(
                GetCounterValue(track_id, snapshot_ts));
          }
        }

        auto process_args_id = process_table.arg_set_id()[upid];
        if (process_args_id) {
          const Json::Value* process_args =
              &args_builder_.GetArgs(process_args_id);
          if (process_args->isMember("is_peak_rss_resettable")) {
            totals["is_peak_rss_resettable"] =
                (*process_args)["is_peak_rss_resettable"];
          }
        }

        const auto& smaps_table = storage_->profiler_smaps_table();
        // Do not create vm_regions without memory maps, since catapult expects
        // to have rows.
        Json::Value* smaps =
            smaps_table.row_count() > 0
                ? &event["args"]["dumps"]["process_mmaps"]["vm_regions"]
                : nullptr;
        for (uint32_t smaps_index = 0; smaps_index < smaps_table.row_count();
             ++smaps_index) {
          if (smaps_table.upid()[smaps_index] != upid)
            continue;
          if (smaps_table.ts()[smaps_index] != snapshot_ts)
            continue;
          Json::Value region;
          region["mf"] =
              GetNonNullString(storage_, smaps_table.file_name()[smaps_index]);
          region["pf"] =
              Json::Int64(smaps_table.protection_flags()[smaps_index]);
          region["sa"] = base::Uint64ToHexStringNoPrefix(
              static_cast<uint64_t>(smaps_table.start_address()[smaps_index]));
          region["sz"] = base::Uint64ToHexStringNoPrefix(
              static_cast<uint64_t>(smaps_table.size_kb()[smaps_index]) * 1024);
          region["ts"] =
              Json::Int64(smaps_table.module_timestamp()[smaps_index]);
          region["id"] = GetNonNullString(
              storage_, smaps_table.module_debugid()[smaps_index]);
          region["df"] = GetNonNullString(
              storage_, smaps_table.module_debug_path()[smaps_index]);
          region["bs"]["pc"] = base::Uint64ToHexStringNoPrefix(
              static_cast<uint64_t>(
                  smaps_table.private_clean_resident_kb()[smaps_index]) *
              1024);
          region["bs"]["pd"] = base::Uint64ToHexStringNoPrefix(
              static_cast<uint64_t>(
                  smaps_table.private_dirty_kb()[smaps_index]) *
              1024);
          region["bs"]["pss"] = base::Uint64ToHexStringNoPrefix(
              static_cast<uint64_t>(
                  smaps_table.proportional_resident_kb()[smaps_index]) *
              1024);
          region["bs"]["sc"] = base::Uint64ToHexStringNoPrefix(
              static_cast<uint64_t>(
                  smaps_table.shared_clean_resident_kb()[smaps_index]) *
              1024);
          region["bs"]["sd"] = base::Uint64ToHexStringNoPrefix(
              static_cast<uint64_t>(
                  smaps_table.shared_dirty_resident_kb()[smaps_index]) *
              1024);
          region["bs"]["sw"] = base::Uint64ToHexStringNoPrefix(
              static_cast<uint64_t>(smaps_table.swap_kb()[smaps_index]) * 1024);
          smaps->append(region);
        }

        if (!totals.empty() || (smaps && !smaps->empty()))
          writer_.WriteCommonEvent(event);
      }

      // Export chrome dump events for process snapshots in current memory
      // snapshot.
      const auto& process_snapshots = storage_->process_memory_snapshot_table();

      for (uint32_t process_index = 0;
           process_index < process_snapshots.row_count(); ++process_index) {
        if (process_snapshots.snapshot_id()[process_index].value != snapshot_id)
          continue;

        auto process_snapshot_id = process_snapshots.id()[process_index].value;
        uint32_t pid = UpidToPid(process_snapshots.upid()[process_index]);

        // Shared memory nodes are imported into a fake process with pid 0.
        // Catapult expects them to be associated with one of the real processes
        // of the snapshot, so we choose the first one we can find and replace
        // the pid.
        if (pid == 0) {
          for (uint32_t i = 0; i < process_snapshots.row_count(); ++i) {
            if (process_snapshots.snapshot_id()[i].value != snapshot_id)
              continue;
            uint32_t new_pid = UpidToPid(process_snapshots.upid()[i]);
            if (new_pid != 0) {
              pid = new_pid;
              break;
            }
          }
        }

        Json::Value event = FillInProcessEventDetails(event_base, pid);

        const auto& snapshot_nodes = storage_->memory_snapshot_node_table();

        for (uint32_t node_index = 0; node_index < snapshot_nodes.row_count();
             ++node_index) {
          if (snapshot_nodes.process_snapshot_id()[node_index].value !=
              process_snapshot_id) {
            continue;
          }
          const char* path =
              GetNonNullString(storage_, snapshot_nodes.path()[node_index]);
          event["args"]["dumps"]["allocators"][path]["guid"] =
              base::Uint64ToHexStringNoPrefix(
                  static_cast<uint64_t>(snapshot_nodes.id()[node_index].value));
          if (snapshot_nodes.size()[node_index]) {
            AddAttributeToMemoryNode(&event, path, "size",
                                     snapshot_nodes.size()[node_index],
                                     "bytes");
          }
          if (snapshot_nodes.effective_size()[node_index]) {
            AddAttributeToMemoryNode(
                &event, path, "effective_size",
                snapshot_nodes.effective_size()[node_index], "bytes");
          }

          auto node_args_id = snapshot_nodes.arg_set_id()[node_index];
          if (!node_args_id)
            continue;
          const Json::Value* node_args =
              &args_builder_.GetArgs(node_args_id.value());
          for (const auto& arg_name : node_args->getMemberNames()) {
            const Json::Value& arg_value = (*node_args)[arg_name]["value"];
            if (arg_value.empty())
              continue;
            if (arg_value.isString()) {
              AddAttributeToMemoryNode(&event, path, arg_name,
                                       arg_value.asString());
            } else if (arg_value.isInt64()) {
              Json::Value unit = (*node_args)[arg_name]["unit"];
              if (unit.empty())
                unit = "unknown";
              AddAttributeToMemoryNode(&event, path, arg_name,
                                       arg_value.asInt64(), unit.asString());
            }
          }
        }

        const auto& snapshot_edges = storage_->memory_snapshot_edge_table();

        for (uint32_t edge_index = 0; edge_index < snapshot_edges.row_count();
             ++edge_index) {
          SnapshotNodeId source_node_id =
              snapshot_edges.source_node_id()[edge_index];
          uint32_t source_node_row =
              *snapshot_nodes.id().IndexOf(source_node_id);

          if (snapshot_nodes.process_snapshot_id()[source_node_row].value !=
              process_snapshot_id) {
            continue;
          }
          Json::Value edge;
          edge["source"] = base::Uint64ToHexStringNoPrefix(
              snapshot_edges.source_node_id()[edge_index].value);
          edge["target"] = base::Uint64ToHexStringNoPrefix(
              snapshot_edges.target_node_id()[edge_index].value);
          edge["importance"] =
              Json::Int(snapshot_edges.importance()[edge_index]);
          edge["type"] = "ownership";
          event["args"]["dumps"]["allocators_graph"].append(edge);
        }
        writer_.WriteCommonEvent(event);
      }
    }
    return util::OkStatus();
  }

  uint32_t UpidToPid(UniquePid upid) {
    auto pid_it = upids_to_exported_pids_.find(upid);
    PERFETTO_DCHECK(pid_it != upids_to_exported_pids_.end());
    return pid_it->second;
  }

  std::pair<uint32_t, uint32_t> UtidToPidAndTid(UniqueTid utid) {
    auto pid_and_tid_it = utids_to_exported_pids_and_tids_.find(utid);
    PERFETTO_DCHECK(pid_and_tid_it != utids_to_exported_pids_and_tids_.end());
    return pid_and_tid_it->second;
  }

  uint32_t NextExportedPidOrTidForDuplicates() {
    // Ensure that the exported substitute value does not represent a valid
    // pid/tid. This would be very unlikely in practice.
    while (IsValidPidOrTid(next_exported_pid_or_tid_for_duplicates_))
      next_exported_pid_or_tid_for_duplicates_--;
    return next_exported_pid_or_tid_for_duplicates_--;
  }

  bool IsValidPidOrTid(uint32_t pid_or_tid) {
    const auto& process_table = storage_->process_table();
    for (UniquePid upid = 0; upid < process_table.row_count(); upid++) {
      if (process_table.pid()[upid] == pid_or_tid)
        return true;
    }

    const auto& thread_table = storage_->thread_table();
    for (UniqueTid utid = 0; utid < thread_table.row_count(); utid++) {
      if (thread_table.tid()[utid] == pid_or_tid)
        return true;
    }

    return false;
  }

  Json::Value FillInProcessEventDetails(const Json::Value& event,
                                        uint32_t pid) {
    Json::Value output = event;
    output["pid"] = Json::Int(pid);
    output["tid"] = Json::Int(-1);
    return output;
  }

  void AddAttributeToMemoryNode(Json::Value* event,
                                const std::string& path,
                                const std::string& key,
                                int64_t value,
                                const std::string& units) {
    (*event)["args"]["dumps"]["allocators"][path]["attrs"][key]["value"] =
        base::Uint64ToHexStringNoPrefix(static_cast<uint64_t>(value));
    (*event)["args"]["dumps"]["allocators"][path]["attrs"][key]["type"] =
        "scalar";
    (*event)["args"]["dumps"]["allocators"][path]["attrs"][key]["units"] =
        units;
  }

  void AddAttributeToMemoryNode(Json::Value* event,
                                const std::string& path,
                                const std::string& key,
                                const std::string& value,
                                const std::string& units = "") {
    (*event)["args"]["dumps"]["allocators"][path]["attrs"][key]["value"] =
        value;
    (*event)["args"]["dumps"]["allocators"][path]["attrs"][key]["type"] =
        "string";
    (*event)["args"]["dumps"]["allocators"][path]["attrs"][key]["units"] =
        units;
  }

  uint64_t GetCounterValue(TrackId track_id, int64_t ts) {
    const auto& counter_table = storage_->counter_table();
    auto begin = counter_table.ts().begin();
    auto end = counter_table.ts().end();
    PERFETTO_DCHECK(counter_table.ts().IsSorted() &&
                    counter_table.ts().IsColumnType<int64_t>());
    // The timestamp column is sorted, so we can binary search for a matching
    // timestamp. Note that we don't use RowMap operations like FilterInto()
    // here because they bloat trace processor's binary size in Chrome too much.
    auto it = std::lower_bound(begin, end, ts,
                               [](const SqlValue& value, int64_t expected_ts) {
                                 return value.AsLong() < expected_ts;
                               });
    for (; it < end; ++it) {
      if ((*it).AsLong() != ts)
        break;
      if (counter_table.track_id()[it.row()].value == track_id.value)
        return static_cast<uint64_t>(counter_table.value()[it.row()]);
    }
    return 0;
  }

  const TraceStorage* storage_;
  ArgsBuilder args_builder_;
  TraceFormatWriter writer_;

  // If a pid/tid is duplicated between two or more  different processes/threads
  // (pid/tid reuse), we export the subsequent occurrences with different
  // pids/tids that is visibly different from regular pids/tids - counting down
  // from uint32_t max.
  uint32_t next_exported_pid_or_tid_for_duplicates_ =
      std::numeric_limits<uint32_t>::max();

  std::map<UniquePid, uint32_t> upids_to_exported_pids_;
  std::map<uint32_t, UniquePid> exported_pids_to_upids_;
  std::map<UniqueTid, std::pair<uint32_t, uint32_t>>
      utids_to_exported_pids_and_tids_;
  std::map<std::pair<uint32_t, uint32_t>, UniqueTid>
      exported_pids_and_tids_to_utids_;
};

#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)

}  // namespace

OutputWriter::OutputWriter() = default;
OutputWriter::~OutputWriter() = default;

util::Status ExportJson(const TraceStorage* storage,
                        OutputWriter* output,
                        ArgumentFilterPredicate argument_filter,
                        MetadataFilterPredicate metadata_filter,
                        LabelFilterPredicate label_filter) {
#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
  JsonExporter exporter(storage, output, std::move(argument_filter),
                        std::move(metadata_filter), std::move(label_filter));
  return exporter.Export();
#else
  perfetto::base::ignore_result(storage);
  perfetto::base::ignore_result(output);
  perfetto::base::ignore_result(argument_filter);
  perfetto::base::ignore_result(metadata_filter);
  perfetto::base::ignore_result(label_filter);
  return util::ErrStatus("JSON support is not compiled in this build");
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
}

util::Status ExportJson(TraceProcessorStorage* tp,
                        OutputWriter* output,
                        ArgumentFilterPredicate argument_filter,
                        MetadataFilterPredicate metadata_filter,
                        LabelFilterPredicate label_filter) {
  const TraceStorage* storage = reinterpret_cast<TraceProcessorStorageImpl*>(tp)
                                    ->context()
                                    ->storage.get();
  return ExportJson(storage, output, argument_filter, metadata_filter,
                    label_filter);
}

util::Status ExportJson(const TraceStorage* storage, FILE* output) {
  FileWriter writer(output);
  return ExportJson(storage, &writer, nullptr, nullptr, nullptr);
}

}  // namespace json
}  // namespace trace_processor
}  // namespace perfetto
