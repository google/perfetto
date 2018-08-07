/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/json_trace_parser.h"

#include <json/reader.h>
#include <json/value.h>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"
#include "src/trace_processor/blob_reader.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) || \
    PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
#error The JSON trace parser is supported only in the standalone build for now.
#endif

namespace perfetto {
namespace trace_processor {

namespace {
const uint32_t kChunkSize = 65536;

// Parses at most one JSON dictionary and returns a pointer to the end of it,
// or nullptr if no dict could be detected.
// This is to avoid decoding the full trace in memory and reduce heap traffic.
// E.g.  input:  { a:1 b:{ c:2, d:{ e:3 } } } , { a:4, ... },
//       output: [   only this is parsed    ] ^return value points here.
const char* ReadOneJsonDict(const char* start,
                            const char* end,
                            Json::Value* value) {
  int braces = 0;
  const char* dict_begin = nullptr;
  for (const char* s = start; s < end; s++) {
    if (isspace(*s) || *s == ',')
      continue;
    if (*s == '{') {
      if (braces == 0)
        dict_begin = s;
      braces++;
      continue;
    }
    if (*s == '}') {
      if (braces <= 0)
        return nullptr;
      if (--braces > 0)
        continue;
      Json::Reader reader;
      if (!reader.parse(dict_begin, s + 1, *value, /*collectComments=*/false)) {
        PERFETTO_ELOG("JSON error: %s",
                      reader.getFormattedErrorMessages().c_str());
        return nullptr;
      }
      return s + 1;
    }
    // TODO(primiano): skip braces in quoted strings, e.g.: {"foo": "ba{z" }
  }
  return nullptr;
}
}  // namespace

// static
constexpr char JsonTraceParser::kPreamble[];

JsonTraceParser::JsonTraceParser(BlobReader* reader,
                                 TraceProcessorContext* context)
    : reader_(reader), context_(context) {}

JsonTraceParser::~JsonTraceParser() = default;

bool JsonTraceParser::ParseNextChunk() {
  if (!buffer_)
    buffer_.reset(new char[kChunkSize]);
  char* buf = buffer_.get();
  const char* next = buf;

  uint32_t rsize =
      reader_->Read(offset_, kChunkSize, reinterpret_cast<uint8_t*>(buf));
  if (rsize == 0)
    return false;

  if (offset_ == 0) {
    if (strncmp(buf, kPreamble, strlen(kPreamble))) {
      buf[strlen(kPreamble)] = '\0';
      PERFETTO_FATAL("Invalid trace preamble, expecting '%s' got '%s'",
                     kPreamble, buf);
    }
    next += strlen(kPreamble);
  }

  ProcessTracker* procs = context_->process_tracker.get();
  TraceStorage* storage = context_->storage.get();
  TraceStorage::NestableSlices* slices = storage->mutable_nestable_slices();

  while (next < &buf[rsize]) {
    Json::Value value;
    const char* res = ReadOneJsonDict(next, buf + rsize, &value);
    if (!res)
      break;
    next = res;
    auto& ph = value["ph"];
    if (!ph.isString())
      continue;
    char phase = *ph.asCString();
    uint32_t tid = value["tid"].asUInt();
    uint32_t pid = value["pid"].asUInt();
    uint64_t ts = value["ts"].asLargestUInt();
    const char* cat = value["cat"].asCString();
    const char* name = value["name"].asCString();
    StringId cat_id = storage->InternString(cat, strlen(cat));
    StringId name_id = storage->InternString(name, strlen(name));
    UniqueTid utid = procs->UpdateThread(tid, pid);
    std::vector<Slice>& stack = threads_[utid].stack;

    switch (phase) {
      case 'B':  // TRACE_EVENT_BEGIN
        stack.emplace_back(Slice{cat_id, name_id, ts});
        break;
      case 'E':  // TRACE_EVENT_END
        PERFETTO_CHECK(!stack.empty() && stack.back().cat_id == cat_id &&
                       stack.back().name_id == name_id);
        Slice& slice = stack.back();
        if (stack.size() < 0xff) {
          slices->AddSlice(slice.start_ts, ts - slice.start_ts, utid, cat_id,
                           name_id, static_cast<uint8_t>(stack.size()));
        }
        stack.pop_back();
        break;
    }
  }
  offset_ += static_cast<uint64_t>(next - buf);
  return next > buf;
}

}  // namespace trace_processor
}  // namespace perfetto
