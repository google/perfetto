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

#include <inttypes.h>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <stdio.h>

#include "perfetto/ext/base/string_splitter.h"
#include "src/trace_processor/export_json.h"
#include "src/trace_processor/metadata.h"
#include "src/trace_processor/trace_storage.h"

namespace {

class TraceFormatWriter {
 public:
  TraceFormatWriter(FILE* output) : output_(output), first_event_(true) {
    WriteHeader();
  }

  ~TraceFormatWriter() { WriteFooter(); }

  void WriteCompleteEvent(int64_t begin_ts_us,
                          int64_t duration_us,
                          const char* cat,
                          const char* name,
                          uint32_t tid,
                          uint32_t pid,
                          const Json::Value& args) {
    if (!first_event_) {
      fputs(",", output_);
    }
    Json::FastWriter writer;
    Json::Value value;
    value["ph"] = "X";
    value["cat"] = cat;
    value["name"] = name;
    value["tid"] = Json::UInt(tid);
    value["pid"] = Json::UInt(pid);
    value["ts"] = Json::Int64(begin_ts_us);
    value["dur"] = Json::Int64(duration_us);
    value["args"] = args;
    fputs(writer.write(value).c_str(), output_);
    first_event_ = false;
  }

  void WriteInstantEvent(int64_t begin_ts_us,
                         const char* scope,
                         const char* cat,
                         const char* name,
                         uint32_t tid,
                         uint32_t pid,
                         const Json::Value& args) {
    if (!first_event_) {
      fputs(",", output_);
    }
    Json::FastWriter writer;
    Json::Value value;
    value["ph"] = "i";
    value["s"] = scope;
    value["cat"] = cat;
    value["name"] = name;
    value["tid"] = Json::UInt(tid);
    value["pid"] = Json::UInt(pid);
    value["ts"] = Json::Int64(begin_ts_us);
    value["args"] = args;
    fputs(writer.write(value).c_str(), output_);
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

  void WriteAsyncInstant(int64_t begin_ts_us,
                         const char* cat,
                         const char* name,
                         uint32_t pid,
                         const Json::Value& async_id,
                         const Json::Value& args) {
    if (!first_event_) {
      fputs(",", output_);
    }
    Json::FastWriter writer;
    Json::Value value;
    value["pid"] = pid;
    value["ph"] = "n";
    value["cat"] = cat;
    value["name"] = name;
    value["id2"] = async_id;
    value["ts"] = Json::Int64(begin_ts_us);
    value["args"] = args;
    fputs(writer.write(value).c_str(), output_);
    first_event_ = false;
  }

  void WriteAsyncStartAndEnd(int64_t begin_ts_us,
                             int64_t duration_us,
                             const char* cat,
                             const char* name,
                             uint32_t pid,
                             const Json::Value& async_id,
                             const Json::Value& args) {
    if (!first_event_) {
      fputs(",", output_);
    }
    Json::FastWriter writer;
    Json::Value value;
    value["pid"] = pid;
    value["ph"] = "b";
    value["cat"] = cat;
    value["name"] = name;
    value["id2"] = async_id;
    value["ts"] = Json::Int64(begin_ts_us);
    value["args"] = args;
    fputs(writer.write(value).c_str(), output_);

    fputs(",", output_);
    value["ph"] = "e";
    value["ts"] = Json::Int64(begin_ts_us + duration_us);
    value.removeMember("args");
    fputs(writer.write(value).c_str(), output_);

    first_event_ = false;
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

 private:
  void WriteHeader() { fputs("{\"traceEvents\":[\n", output_); }

  void WriteFooter() {
    if (metadata_.empty()) {
      fputs("]}", output_);
    } else {
      fputs("],\n\"metadata\":", output_);
      Json::FastWriter writer;
      fputs(writer.write(metadata_).c_str(), output_);
      fputs("\n}", output_);
    }
    fflush(output_);
  }

  FILE* output_;
  bool first_event_;
  Json::Value metadata_;
};

std::string PrintUint64(uint64_t x) {
  char hex_str[19];
  sprintf(hex_str, "0x%" PRIx64, x);
  return hex_str;
}

}  // anonymous namespace

namespace perfetto {
namespace trace_processor {
namespace json {

namespace {

class ArgsBuilder {
 public:
  explicit ArgsBuilder(const TraceStorage* storage)
      : string_pool_(&storage->string_pool()) {
    const TraceStorage::Args& args = storage->args();
    Json::Value empty_value(Json::objectValue);
    if (args.args_count() == 0) {
      args_sets_.resize(1, empty_value);
      return;
    }
    args_sets_.resize(args.set_ids().back() + 1, empty_value);
    for (size_t i = 0; i < args.args_count(); ++i) {
      ArgSetId set_id = args.set_ids()[i];
      const char* key = string_pool_->Get(args.keys()[i]).c_str();
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
        return string_pool_->Get(variadic.string_value).c_str();
      case Variadic::kReal:
        return variadic.real_value;
      case Variadic::kPointer:
        return PrintUint64(variadic.pointer_value);
      case Variadic::kBool:
        return variadic.bool_value;
      case Variadic::kJson:
        Json::Reader reader;
        Json::Value result;
        reader.parse(string_pool_->Get(variadic.string_value).c_str(), result);
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

  const StringPool* string_pool_;
  std::vector<Json::Value> args_sets_;
};

ResultCode ExportThreadNames(const TraceStorage* storage,
                             TraceFormatWriter* writer) {
  const StringPool& string_pool = storage->string_pool();
  for (UniqueTid i = 1; i < storage->thread_count(); ++i) {
    auto thread = storage->GetThread(i);
    if (thread.name_id > 0) {
      const char* thread_name = string_pool.Get(thread.name_id).c_str();
      uint32_t pid = thread.upid ? storage->GetProcess(*thread.upid).pid : 0;
      writer->WriteMetadataEvent("thread_name", thread_name, thread.tid, pid);
    }
  }
  return kResultOk;
}

ResultCode ExportProcessNames(const TraceStorage* storage,
                              TraceFormatWriter* writer) {
  const StringPool& string_pool = storage->string_pool();
  for (UniquePid i = 1; i < storage->process_count(); ++i) {
    auto process = storage->GetProcess(i);
    if (process.name_id > 0) {
      const char* process_name = string_pool.Get(process.name_id).c_str();
      writer->WriteMetadataEvent("process_name", process_name, 0, process.pid);
    }
  }
  return kResultOk;
}

ResultCode ExportSlices(const TraceStorage* storage,
                        TraceFormatWriter* writer) {
  const StringPool& string_pool = storage->string_pool();
  ArgsBuilder args_builder(storage);
  const auto& slices = storage->nestable_slices();
  for (size_t i = 0; i < slices.slice_count(); ++i) {
    int64_t begin_ts_us = slices.start_ns()[i] / 1000;
    int64_t duration_us = slices.durations()[i] / 1000;
    const char* cat = string_pool.Get(slices.categories()[i]).c_str();
    const char* name = string_pool.Get(slices.names()[i]).c_str();
    Json::Value args = args_builder.GetArgs(slices.arg_set_ids()[i]);

    if (slices.types()[i] == RefType::kRefTrack) {  // Async event.
      uint32_t track_id = static_cast<uint32_t>(slices.refs()[i]);
      VirtualTrackScope scope = storage->virtual_tracks().scopes()[track_id];
      UniquePid upid = storage->virtual_tracks().upids()[track_id];
      Json::Value async_id;
      uint32_t pid = 0;
      if (scope == VirtualTrackScope::kGlobal) {
        async_id["global"] = PrintUint64(track_id);
      } else {
        async_id["local"] = PrintUint64(track_id);
        pid = storage->GetProcess(upid).pid;
      }
      if (slices.durations()[i] == 0) {  // Instant async event.
        writer->WriteAsyncInstant(begin_ts_us, cat, name, pid, async_id, args);
      } else {  // Async start and end.
        writer->WriteAsyncStartAndEnd(begin_ts_us, duration_us, cat, name, pid,
                                      async_id, args);
      }
    } else {                             // Sync event.
      if (slices.durations()[i] == 0) {  // Instant event.
        uint32_t pid = 0;
        uint32_t tid = 0;
        std::string instant_scope;
        if (slices.types()[i] == RefType::kRefUtid) {
          UniqueTid utid = static_cast<UniqueTid>(slices.refs()[i]);
          auto thread = storage->GetThread(utid);
          pid = thread.upid ? storage->GetProcess(*thread.upid).pid : 0;
          tid = thread.tid;
          instant_scope = "t";
        } else if (slices.types()[i] == RefType::kRefUpid) {
          UniquePid upid = static_cast<UniquePid>(slices.refs()[i]);
          pid = storage->GetProcess(upid).pid;
          instant_scope = "p";
        } else if (slices.types()[i] == RefType::kRefNoRef) {
          instant_scope = "g";
        } else {
          return kResultWrongRefType;
        }
        writer->WriteInstantEvent(begin_ts_us, instant_scope.c_str(), cat, name,
                                  tid, pid, args);
      } else {  // Complete event.
        if (slices.types()[i] != RefType::kRefUtid) {
          return kResultWrongRefType;
        }
        UniqueTid utid = static_cast<UniqueTid>(slices.refs()[i]);
        auto thread = storage->GetThread(utid);
        uint32_t pid = thread.upid ? storage->GetProcess(*thread.upid).pid : 0;
        writer->WriteCompleteEvent(begin_ts_us, duration_us, cat, name,
                                   thread.tid, pid, args);
      }
    }
  }
  return kResultOk;
}

ResultCode ExportMetadata(const TraceStorage* storage,
                          TraceFormatWriter* writer) {
  const StringPool& string_pool = storage->string_pool();
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
            string_pool.Get(values[pos].string_value).c_str());
        break;

      case metadata::benchmark_name:
        writer->AppendTelemetryMetadataString(
            "benchmarks", string_pool.Get(values[pos].string_value).c_str());
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
            "labels", string_pool.Get(values[pos].string_value).c_str());
        break;

      case metadata::benchmark_story_name:
        writer->AppendTelemetryMetadataString(
            "stories", string_pool.Get(values[pos].string_value).c_str());
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
            "storyTags", string_pool.Get(values[pos].string_value).c_str());
        break;

      default:
        PERFETTO_DFATAL("unexpected metadata key");
        break;
    }
  }
  return kResultOk;
}

}  // anonymous namespace

ResultCode ExportJson(const TraceStorage* storage, FILE* output) {
  TraceFormatWriter writer(output);

  ResultCode code = ExportThreadNames(storage, &writer);
  if (code != kResultOk)
    return code;

  code = ExportProcessNames(storage, &writer);
  if (code != kResultOk)
    return code;

  code = ExportSlices(storage, &writer);
  if (code != kResultOk)
    return code;

  code = ExportMetadata(storage, &writer);
  if (code != kResultOk)
    return code;

  return kResultOk;
}

}  // namespace json
}  // namespace trace_processor
}  // namespace perfetto
