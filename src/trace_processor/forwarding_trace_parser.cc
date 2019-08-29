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

#include "src/trace_processor/forwarding_trace_parser.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/fuchsia_trace_parser.h"
#include "src/trace_processor/fuchsia_trace_tokenizer.h"
#include "src/trace_processor/gzip_trace_parser.h"
#include "src/trace_processor/proto_trace_parser.h"
#include "src/trace_processor/proto_trace_tokenizer.h"
#include "src/trace_processor/systrace_trace_parser.h"

// JSON parsing and exporting is only supported in the standalone and
// Chromium builds.
#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
#include "src/trace_processor/json_trace_parser.h"
#include "src/trace_processor/json_trace_tokenizer.h"
#endif

namespace perfetto {
namespace trace_processor {
namespace {

std::string RemoveWhitespace(const std::string& input) {
  std::string str(input);
  str.erase(std::remove_if(str.begin(), str.end(), ::isspace), str.end());
  return str;
}

// Fuchsia traces have a magic number as documented here:
// https://fuchsia.googlesource.com/fuchsia/+/HEAD/docs/development/tracing/trace-format/README.md#magic-number-record-trace-info-type-0
constexpr uint64_t kFuchsiaMagicNumber = 0x0016547846040010;

}  // namespace

ForwardingTraceParser::ForwardingTraceParser(TraceProcessorContext* context)
    : context_(context) {}

ForwardingTraceParser::~ForwardingTraceParser() {}

util::Status ForwardingTraceParser::Parse(std::unique_ptr<uint8_t[]> data,
                                          size_t size) {
  // If this is the first Parse() call, guess the trace type and create the
  // appropriate parser.

  if (!reader_) {
    TraceType trace_type;
    {
      auto scoped_trace = context_->storage->TraceExecutionTimeIntoStats(
          stats::guess_trace_type_duration_ns);
      trace_type = GuessTraceType(data.get(), size);
    }
    switch (trace_type) {
      case kJsonTraceType: {
        PERFETTO_DLOG("JSON trace detected");
#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
        reader_.reset(new JsonTraceTokenizer(context_));
        // JSON traces have no guarantees about the order of events in them.
        int64_t window_size_ns = std::numeric_limits<int64_t>::max();
        context_->sorter.reset(new TraceSorter(context_, window_size_ns));
        context_->parser.reset(new JsonTraceParser(context_));
#else
        PERFETTO_FATAL("JSON traces not supported.");
#endif
        break;
      }
      case kProtoTraceType: {
        PERFETTO_DLOG("Proto trace detected");
        // This will be reduced once we read the trace config and we see flush
        // period being set.
        int64_t window_size_ns = std::numeric_limits<int64_t>::max();
        reader_.reset(new ProtoTraceTokenizer(context_));
        context_->sorter.reset(new TraceSorter(context_, window_size_ns));
        context_->parser.reset(new ProtoTraceParser(context_));
        break;
      }
      case kFuchsiaTraceType: {
        PERFETTO_DLOG("Fuchsia trace detected");
        // Fuschia traces can have massively out of order events.
        int64_t window_size_ns = std::numeric_limits<int64_t>::max();
        reader_.reset(new FuchsiaTraceTokenizer(context_));
        context_->sorter.reset(new TraceSorter(context_, window_size_ns));
        context_->parser.reset(new FuchsiaTraceParser(context_));
        break;
      }
      case kSystraceTraceType:
        PERFETTO_DLOG("Systrace trace detected");
        reader_.reset(new SystraceTraceParser(context_));
        break;
      case kGzipTraceType:
        PERFETTO_DLOG("gzip trace detected");
        reader_.reset(new GzipTraceParser(context_));
        break;
      case kCtraceTraceType:
        PERFETTO_DLOG("ctrace trace detected");
        reader_.reset(new GzipTraceParser(context_));
        break;
      case kUnknownTraceType:
        return util::ErrStatus("Unknown trace type provided");
    }
  }

  return reader_->Parse(std::move(data), size);
}

TraceType GuessTraceType(const uint8_t* data, size_t size) {
  if (size == 0)
    return kUnknownTraceType;
  std::string start(reinterpret_cast<const char*>(data),
                    std::min<size_t>(size, 20));
  std::string start_minus_white_space = RemoveWhitespace(start);
  if (base::StartsWith(start_minus_white_space, "{\"traceEvents\":["))
    return kJsonTraceType;
  if (base::StartsWith(start_minus_white_space, "[{"))
    return kJsonTraceType;
  if (size >= 8) {
    uint64_t first_word = *reinterpret_cast<const uint64_t*>(data);
    if (first_word == kFuchsiaMagicNumber)
      return kFuchsiaTraceType;
  }

  // Systrace with header but no leading HTML.
  if (base::StartsWith(start, "# tracer"))
    return kSystraceTraceType;

  // Systrace with leading HTML.
  if (base::StartsWith(start, "<!DOCTYPE html>") ||
      base::StartsWith(start, "<html>"))
    return kSystraceTraceType;

  // Systrace with no header or leading HTML.
  if (base::StartsWith(start, " "))
    return kSystraceTraceType;

  // Ctrace is deflate'ed systrace.
  if (base::StartsWith(start, "TRACE:"))
    return kCtraceTraceType;

  // gzip'ed trace containing one of the other formats.
  if (base::StartsWith(start, "\x1f\x8b"))
    return kGzipTraceType;

  return kProtoTraceType;
}

}  // namespace trace_processor
}  // namespace perfetto
