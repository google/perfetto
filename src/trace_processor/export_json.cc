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

#include "src/trace_processor/export_json.h"

#include <json/value.h>
#include <json/writer.h>
#include <stdio.h>

#include "src/trace_processor/trace_storage.h"

namespace {

class TraceFormatWriter {
 public:
  TraceFormatWriter(FILE* output) : output_(output), first_event_(true) {
    WriteHeader();
  }

  ~TraceFormatWriter() { WriteFooter(); }

  void WriteSlice(int64_t begin_ts_us,
                  int64_t duration_us,
                  const char* cat,
                  const char* name,
                  uint32_t tid,
                  uint32_t pid) {
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

 private:
  void WriteHeader() { fputs("{\"traceEvents\":[\n", output_); }

  void WriteFooter() {
    fputs("]}\n", output_);
    fflush(output_);
  }

  FILE* output_;
  bool first_event_;
};

}  // anonymous namespace

namespace perfetto {
namespace trace_processor {
namespace json {

ResultCode ExportJson(const TraceStorage* storage, FILE* output) {
  const StringPool& string_pool = storage->string_pool();

  TraceFormatWriter writer(output);

  // Write thread names.
  for (UniqueTid i = 1; i < storage->thread_count(); ++i) {
    auto thread = storage->GetThread(i);
    if (thread.name_id > 0) {
      const char* thread_name = string_pool.Get(thread.name_id).c_str();
      uint32_t pid = thread.upid ? storage->GetProcess(*thread.upid).pid : 0;
      writer.WriteMetadataEvent("thread_name", thread_name, thread.tid, pid);
    }
  }

  // Write process names.
  for (UniquePid i = 1; i < storage->process_count(); ++i) {
    auto process = storage->GetProcess(i);
    if (process.name_id > 0) {
      const char* process_name = string_pool.Get(process.name_id).c_str();
      writer.WriteMetadataEvent("process_name", process_name, 0, process.pid);
    }
  }

  // Write slices.
  const auto& slices = storage->nestable_slices();
  for (size_t i = 0; i < slices.slice_count(); ++i) {
    if (slices.types()[i] == RefType::kRefUtid) {
      int64_t begin_ts_us = slices.start_ns()[i] / 1000;
      int64_t duration_us = slices.durations()[i] / 1000;
      const char* cat = string_pool.Get(slices.cats()[i]).c_str();
      const char* name = string_pool.Get(slices.names()[i]).c_str();

      UniqueTid utid = static_cast<UniqueTid>(slices.refs()[i]);
      auto thread = storage->GetThread(utid);
      uint32_t pid = thread.upid ? storage->GetProcess(*thread.upid).pid : 0;

      writer.WriteSlice(begin_ts_us, duration_us, cat, name, thread.tid, pid);
    } else {
      return kResultWrongRefType;
    }
  }
  return kResultOk;
}

}  // namespace json
}  // namespace trace_processor
}  // namespace perfetto
