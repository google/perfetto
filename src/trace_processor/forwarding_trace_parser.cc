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

#include <memory>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/trace_reader_registry.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_processor/util/trace_type.h"

namespace perfetto::trace_processor {
namespace {

TraceSorter::SortingMode ConvertSortingMode(SortingMode sorting_mode) {
  switch (sorting_mode) {
    case SortingMode::kDefaultHeuristics:
      return TraceSorter::SortingMode::kDefault;
    case SortingMode::kForceFullSort:
      return TraceSorter::SortingMode::kFullSort;
  }
  PERFETTO_FATAL("For GCC");
}

std::optional<TraceSorter::SortingMode> GetMinimumSortingMode(
    TraceType trace_type,
    const TraceProcessorContext& context) {
  switch (trace_type) {
    case kNinjaLogTraceType:
    case kSystraceTraceType:
    case kGzipTraceType:
    case kCtraceTraceType:
      return std::nullopt;

    case kPerfDataTraceType:
    case kInstrumentsXmlTraceType:
      return TraceSorter::SortingMode::kDefault;

    case kUnknownTraceType:
    case kJsonTraceType:
    case kFuchsiaTraceType:
    case kZipFile:
    case kAndroidLogcatTraceType:
    case kGeckoTraceType:
    case kArtMethodTraceType:
    case kPerfTextTraceType:
      return TraceSorter::SortingMode::kFullSort;

    case kProtoTraceType:
    case kSymbolsTraceType:
      return ConvertSortingMode(context.config.sorting_mode);

    case kAndroidDumpstateTraceType:
    case kAndroidBugreportTraceType:
      PERFETTO_FATAL(
          "This trace type should be handled at the ZipParser level");
  }
  PERFETTO_FATAL("For GCC");
}

}  // namespace

ForwardingTraceParser::ForwardingTraceParser(TraceProcessorContext* context)
    : context_(context) {}

ForwardingTraceParser::~ForwardingTraceParser() {}

base::Status ForwardingTraceParser::Init(const TraceBlobView& blob) {
  PERFETTO_CHECK(!reader_);

  {
    auto scoped_trace = context_->storage->TraceExecutionTimeIntoStats(
        stats::guess_trace_type_duration_ns);
    trace_type_ = GuessTraceType(blob.data(), blob.size());
  }
  if (trace_type_ == kUnknownTraceType) {
    // If renaming this error message don't remove the "(ERR:fmt)" part.
    // The UI's error_dialog.ts uses it to make the dialog more graceful.
    return base::ErrStatus("Unknown trace type provided (ERR:fmt)");
  }

  base::StatusOr<std::unique_ptr<ChunkedTraceReader>> reader_or =
      context_->reader_registry->CreateTraceReader(trace_type_);
  if (!reader_or.ok()) {
    return reader_or.status();
  }
  reader_ = std::move(*reader_or);

  PERFETTO_DLOG("%s trace detected", TraceTypeToString(trace_type_));
  UpdateSorterForTraceType(trace_type_);

  // TODO(b/334978369) Make sure kProtoTraceType and kSystraceTraceType are
  // parsed first so that we do not get issues with
  // SetPidZeroIsUpidZeroIdleProcess()
  if (trace_type_ == kProtoTraceType || trace_type_ == kSystraceTraceType) {
    context_->process_tracker->SetPidZeroIsUpidZeroIdleProcess();
  }

  return base::OkStatus();
}

void ForwardingTraceParser::UpdateSorterForTraceType(TraceType trace_type) {
  std::optional<TraceSorter::SortingMode> minimum_sorting_mode =
      GetMinimumSortingMode(trace_type, *context_);
  if (!minimum_sorting_mode.has_value()) {
    return;
  }

  if (!context_->sorter) {
    context_->sorter.reset(new TraceSorter(context_, *minimum_sorting_mode));
  }

  switch (context_->sorter->sorting_mode()) {
    case TraceSorter::SortingMode::kDefault:
      PERFETTO_CHECK(minimum_sorting_mode ==
                     TraceSorter::SortingMode::kDefault);
      break;
    case TraceSorter::SortingMode::kFullSort:
      break;
  }
}

base::Status ForwardingTraceParser::Parse(TraceBlobView blob) {
  // If this is the first Parse() call, guess the trace type and create the
  // appropriate parser.
  if (!reader_) {
    RETURN_IF_ERROR(Init(blob));
  }
  return reader_->Parse(std::move(blob));
}

base::Status ForwardingTraceParser::NotifyEndOfFile() {
  return reader_ ? reader_->NotifyEndOfFile() : base::OkStatus();
}

}  // namespace perfetto::trace_processor
