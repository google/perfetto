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

#include <limits>
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
const uint32_t kChunkSize = 1024 * 512;

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
    SlicesStack& stack = threads_[utid];

    auto add_slice = [slices, &stack, utid, cat_id,
                      name_id](const Slice& slice) {
      if (stack.size() >= std::numeric_limits<uint8_t>::max())
        return;
      const uint8_t depth = static_cast<uint8_t>(stack.size()) - 1;
      uint64_t parent_stack_id, stack_id;
      std::tie(parent_stack_id, stack_id) = GetStackHashes(stack);
      slices->AddSlice(slice.start_ts, slice.end_ts - slice.start_ts, utid,
                       cat_id, name_id, depth, stack_id, parent_stack_id);
    };

    switch (phase) {
      case 'B': {  // TRACE_EVENT_BEGIN.
        MaybeCloseStack(ts, stack);
        stack.emplace_back(Slice{cat_id, name_id, ts, 0});
        break;
      }
      case 'E': {  // TRACE_EVENT_END.
        PERFETTO_CHECK(!stack.empty());
        MaybeCloseStack(ts, stack);
        PERFETTO_CHECK(stack.back().cat_id == cat_id);
        PERFETTO_CHECK(stack.back().name_id == name_id);
        Slice& slice = stack.back();
        slice.end_ts = slice.start_ts;
        add_slice(slice);
        stack.pop_back();
        break;
      }
      case 'X': {  // TRACE_EVENT (scoped event).
        MaybeCloseStack(ts, stack);
        uint64_t end_ts = ts + value["dur"].asUInt();
        stack.emplace_back(Slice{cat_id, name_id, ts, end_ts});
        Slice& slice = stack.back();
        add_slice(slice);
        break;
      }
      case 'M': {  // Metadata events (process and thread names).
        if (strcmp(value["name"].asCString(), "thread_name") == 0) {
          const char* thread_name = value["args"]["name"].asCString();
          procs->UpdateThreadName(tid, pid, thread_name, strlen(thread_name));
          break;
        }
        if (strcmp(value["name"].asCString(), "process_name") == 0) {
          const char* proc_name = value["args"]["name"].asCString();
          procs->UpdateProcess(pid, proc_name, strlen(proc_name));
          break;
        }
      }
    }
    // TODO(primiano): auto-close B slices left open at the end.
  }
  offset_ += static_cast<uint64_t>(next - buf);
  return next > buf;
}

void JsonTraceParser::MaybeCloseStack(uint64_t ts, SlicesStack& stack) {
  bool check_only = false;
  for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
    const Slice& slice = stack[size_t(i)];
    if (slice.end_ts == 0) {
      check_only = true;
    }

    if (check_only) {
      PERFETTO_DCHECK(ts >= slice.start_ts);
      PERFETTO_DCHECK(slice.end_ts == 0 || ts <= slice.end_ts);
      continue;
    }

    if (slice.end_ts <= ts) {
      stack.pop_back();
    }
  }
}

// Returns <parent_stack_id, stack_id>, where
// |parent_stack_id| == hash(stack_id - last slice).
std::tuple<uint64_t, uint64_t> JsonTraceParser::GetStackHashes(
    const SlicesStack& stack) {
  PERFETTO_DCHECK(!stack.empty());
  std::string s;
  s.reserve(stack.size() * sizeof(uint64_t) * 2);
  constexpr uint64_t kMask = uint64_t(-1) >> 1;
  uint64_t parent_stack_id = 0;
  for (size_t i = 0; i < stack.size(); i++) {
    if (i == stack.size() - 1)
      parent_stack_id = i > 0 ? (std::hash<std::string>{}(s)) & kMask : 0;
    const Slice& slice = stack[i];
    s.append(reinterpret_cast<const char*>(&slice.cat_id),
             sizeof(slice.cat_id));
    s.append(reinterpret_cast<const char*>(&slice.name_id),
             sizeof(slice.name_id));
  }
  uint64_t stack_id = (std::hash<std::string>{}(s)) & kMask;
  return std::make_tuple(parent_stack_id, stack_id);
}

}  // namespace trace_processor
}  // namespace perfetto
