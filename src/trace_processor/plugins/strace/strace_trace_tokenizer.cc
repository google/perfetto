/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/plugins/strace/strace_trace_tokenizer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/plugins/strace/strace_event.h"
#include "src/trace_processor/plugins/strace/strace_line_parser.h"
#include "src/trace_processor/plugins/strace/strace_trace_parser.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/clock_synchronizer.h"

namespace perfetto::trace_processor::strace_importer {

namespace {

std::string_view ToStringView(const TraceBlobView& tbv) {
  return {reinterpret_cast<const char*>(tbv.data()), tbv.size()};
}

}  // namespace

StraceTraceTokenizer::StraceTraceTokenizer(TraceProcessorContext* ctx)
    : context_(ctx),
      stream_(
          ctx->sorter->CreateStream(std::make_unique<StraceTraceParser>(ctx))) {
}
StraceTraceTokenizer::~StraceTraceTokenizer() = default;

base::Status StraceTraceTokenizer::Parse(TraceBlobView blob) {
  reader_.PushBack(std::move(blob));
  for (;;) {
    auto it = reader_.GetIterator();
    auto r = it.MaybeFindAndRead('\n');
    if (!r) {
      return base::OkStatus();
    }
    std::string_view line = ToStringView(*r);
    reader_.PopFrontUntil(it.file_offset());

    std::optional<StraceLine> parsed = ParseStraceLine(line);
    if (!parsed) {
      // Not every line in an strace log is a syscall (signal delivery,
      // process exit banners, etc). Skip anything we don't recognise
      // rather than treating the whole trace as invalid.
      continue;
    }

    std::optional<int64_t> trace_ts =
        context_->clock_tracker->ConvertDefaultClockToTraceTime(parsed->tod_ns);
    if (!trace_ts) {
      continue;
    }

    StraceEvent evt;
    evt.tid = parsed->pid.value_or(1);
    evt.syscall_name_id =
        context_->storage->InternString(base::StringView(parsed->syscall));
    if (!parsed->args.empty()) {
      evt.args_id =
          context_->storage->InternString(base::StringView(parsed->args));
    }
    if (parsed->return_value) {
      evt.return_value_id = context_->storage->InternString(
          base::StringView(*parsed->return_value));
    }
    evt.is_unfinished = parsed->is_unfinished;
    evt.is_resumed = parsed->is_resumed;

    stream_->Push(*trace_ts, evt);
  }
}

}  // namespace perfetto::trace_processor::strace_importer
