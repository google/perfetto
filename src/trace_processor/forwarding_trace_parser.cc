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

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/trace_reader_registry.h"
#include "src/trace_processor/types/trace_manifest_state.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/clock_synchronizer.h"
#include "src/trace_processor/util/trace_type.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"

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
    case kGzipTraceType:
      return std::nullopt;

    case kAndroidDumpstateTraceType:
    case kAndroidLogcatTraceType:
    case kArtHprofTraceType:
    case kArtMethodTraceType:
    case kArtMethodV2TraceType:
    case kCollapsedStackTraceType:
    case kCtraceTraceType:
    case kFuchsiaTraceType:
    case kGeckoTraceType:
    case kInstrumentsXmlTraceType:
    case kJsonTraceType:
    case kNinjaLogTraceType:
    case kPerfDataTraceType:
    case kPerfTextTraceType:
    case kPerfettoManifestTraceType:
    case kPprofTraceType:
    case kPrimesTraceType:
    case kSimpleperfProtoTraceType:
    case kSystraceTraceType:
    case kTarTraceType:
    case kUnknownTraceType:
    case kZipFile:
      return TraceSorter::SortingMode::kFullSort;

    case kProtoTraceType:
    case kSymbolsTraceType:
      return ConvertSortingMode(context.config.sorting_mode);

    case kAndroidBugreportTraceType:
      PERFETTO_FATAL(
          "This trace type should be handled at the ZipParser level");
  }
  PERFETTO_FATAL("For GCC");
}

}  // namespace

ForwardingTraceParser::ForwardingTraceParser(TraceProcessorContext* context,
                                             tables::TraceFileTable::Id id)
    : input_context_(context), file_id_(id) {}

ForwardingTraceParser::~ForwardingTraceParser() = default;

base::Status ForwardingTraceParser::Init(const TraceBlobView& blob) {
  PERFETTO_CHECK(!reader_);

  {
    auto scoped_trace =
        input_context_->global_stats_tracker->TraceExecutionTimeIntoStats(
            stats::guess_trace_type_duration_ns);
    trace_type_ = GuessTraceType(blob.data(), blob.size());
  }
  if (trace_type_ == kUnknownTraceType) {
    // If renaming this error message don't remove the "(ERR:fmt)" part.
    // The UI's error_dialog.ts uses it to make the dialog more graceful.
    return base::ErrStatus("Unknown trace type provided (ERR:fmt)");
  }
  PERFETTO_DLOG("%s trace detected", TraceTypeToString(trace_type_));

  if (file_id_.value != 0 && trace_type_ == kNinjaLogTraceType) {
    return base::ErrStatus(
        "Ninja traces currently do not support being contained inside other "
        "trace formats. Please file a bug at "
        "https://github.com/google/perfetto/issues if this is important to "
        "you.");
  }

  // A perfetto_manifest file configures the parsing of the files which
  // follow it, so it is only valid before any non-container trace. Archive
  // sorting guarantees this for direct members; this rejects e.g. a
  // gzip-wrapped metadata file sorted after a proto trace.
  if (trace_type_ == kPerfettoManifestTraceType &&
      input_context_->forked_context_state->trace_to_context.size() != 0) {
    return base::ErrStatus(
        "perfetto_manifest file must be the first trace file in the input");
  }

  std::optional<TraceSorter::SortingMode> minimum_sorting_mode =
      GetMinimumSortingMode(trace_type_, *input_context_);
  if (minimum_sorting_mode) {
    input_context_->sorter->SetSortingMode(*minimum_sorting_mode);
  }
  input_context_->trace_file_tracker->StartParsing(file_id_, trace_type_);

  // If the perfetto_manifest file has an entry for this file (matched by
  // exact path), it overrides clock/machine handling below.
  TraceManifestState::FileEntry* manifest_entry = FindManifestEntry();
  if (manifest_entry &&
      (manifest_entry->clock_override || manifest_entry->machine_id) &&
      (IsContainerTraceType(trace_type_) ||
       trace_type_ == kPerfettoManifestTraceType)) {
    return base::ErrStatus(
        "perfetto_manifest: overrides are not supported for trace files "
        "which are themselves archives or perfetto_manifest files: %s",
        manifest_entry->path.c_str());
  }

  if (IsContainerTraceType(trace_type_) ||
      trace_type_ == kPerfettoManifestTraceType) {
    // perfetto_manifest files produce no events: like containers they must
    // not fork a per-trace context, as that would make this file the
    // "primary" trace for its machine and demote the real traces.
    PERFETTO_DCHECK(!input_context_->trace_state);
    trace_context_ = input_context_;
  } else {
    uint32_t raw_machine_id = manifest_entry && manifest_entry->machine_id
                                  ? *manifest_entry->machine_id
                                  : 0;
    // TODO(b/334978369) Make sure kProtoTraceType and kSystraceTraceType are
    // parsed first so that we do not get issues with
    // SetPidZeroIsUpidZeroIdleProcess()
    // The machine row was pre-allocated by the manifest reader (which also
    // named it); this fork reuses it via MachineTracker.
    trace_context_ =
        input_context_->ForkContextForTrace(file_id_, raw_machine_id);
    if (trace_type_ == kProtoTraceType || trace_type_ == kSystraceTraceType) {
      trace_context_->process_tracker->SetPidZeroIsUpidZeroIdleProcess();
    }
    if (manifest_entry) {
      trace_context_->trace_state->has_machine_override =
          manifest_entry->machine_id.has_value();
    }
  }
  ASSIGN_OR_RETURN(reader_, input_context_->reader_registry->CreateTraceReader(
                                trace_type_, trace_context_, file_id_.value));

  // Centralize clock setup for all trace formats. Every format declares the
  // clock domain its native timestamps are expressed in (its "trace clock"),
  // and we do three things with it:
  //
  //   1. Record it as the file's default clock, which tokenizers convert their
  //      events through via ClockTracker::ConvertDefaultClockToTraceTime. Proto
  //      is the exception (see below).
  //
  //   2. Claim it as the global trace-time clock. The first trace to claim
  //      wins; later claims are silently ignored, so the global clock is
  //      stable regardless of how many traces an archive (e.g. a ZIP) holds.
  //
  //   3. Register a deferred clock sync for the same clock (an implicit edge).
  //      If this trace does NOT win the global clock (e.g. a proto trace in the
  //      same archive claimed BOOTTIME first) and the trace clock is not
  //      otherwise linked into the clock graph via a ClockSnapshot, a
  //      zero-offset identity edge is injected on the first conversion. This
  //      keeps the trace's timestamps convertible instead of silently dropping
  //      every event. When a real ClockSnapshot does link the clock (e.g. proto
  //      provides BOOTTIME<->MONOTONIC), that real relationship is used
  //      instead.
  //
  // Proto traces are special: their clock is whatever ParseClockSnapshot reads
  // from primary_trace_clock, set later, so here we only register BOOTTIME as
  // the deferred fallback, do not set the default clock, and do not claim the
  // global clock now.
  //
  // A perfetto_manifest clock override for this file replaces the format's
  // best-effort source clock (below) with the file's own private clock and
  // customizes its implicit edge into the graph (see the manifest branch).
  using ClockId = ClockTracker::ClockId;
  std::optional<ClockId> trace_clock;
  bool claim_global_clock = true;
  if (trace_type_ == kProtoTraceType) {
    trace_clock = ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_BOOTTIME);
    claim_global_clock = false;
  } else if (trace_type_ == kSystraceTraceType ||
             trace_type_ == kSimpleperfProtoTraceType ||
             trace_type_ == kPerfTextTraceType ||
             trace_type_ == kPerfDataTraceType ||
             trace_type_ == kArtMethodTraceType ||
             trace_type_ == kArtMethodV2TraceType) {
    trace_clock = ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
  } else if (trace_type_ == kFuchsiaTraceType) {
    trace_clock = ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_BOOTTIME);
  } else if (trace_type_ == kGeckoTraceType || trace_type_ == kJsonTraceType ||
             trace_type_ == kInstrumentsXmlTraceType ||
             trace_type_ == kPrimesTraceType) {
    trace_clock = ClockId::TraceFile(trace_context_->trace_id().value);
  } else if (trace_type_ == kAndroidDumpstateTraceType ||
             trace_type_ == kAndroidLogcatTraceType) {
    trace_clock = ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_REALTIME);
  }
  auto& clock_tracker = trace_context_->clock_tracker;
  // A "relate" override (source_clock set) is a cross-machine edge that the
  // manifest reader already added to the global graph. The remaining override
  // is the "pin" form below.
  if (manifest_entry && manifest_entry->clock_override &&
      !manifest_entry->clock_override->source_clock) {
    // "Pin" form: the file has no usable clock, so move its events onto a
    // private TraceFile clock and register a single implicit edge connecting
    // that clock to the graph as the manifest dictates. The file is now
    // single-clock / single-machine; stray ClockSnapshots or remote machine
    // ids on it are rejected (see has_clock_override()).
    const TraceManifestState::ClockOverride& clock_override =
        *manifest_entry->clock_override;
    ClockId file_clock = ClockId::TraceFile(trace_context_->trace_id().value);
    clock_tracker->SetTraceDefaultClock(file_clock);
    trace_context_->trace_state->has_clock_override = true;
    if (claim_global_clock) {
      clock_tracker->SetGlobalClock(file_clock);
    }
    // An omitted reference clock means trace time; a named clock means that
    // builtin domain (which the rest of the graph is expected to reach).
    std::optional<ClockId> target =
        clock_override.clock
            ? std::make_optional(ClockId::Machine(*clock_override.clock))
            : std::nullopt;
    clock_tracker->AddDeferredClockSync(file_clock, clock_override.file_ts_ns,
                                        target, clock_override.clock_ts_ns);
  } else if (trace_clock) {
    // Proto manages its own default clock (primary_trace_clock / ClockSnapshot)
    // so it must not be set here; every other format converts its events
    // through the default clock via
    // ClockTracker::ConvertDefaultClockToTraceTime.
    if (trace_type_ != kProtoTraceType) {
      clock_tracker->SetTraceDefaultClock(*trace_clock);
    }
    if (claim_global_clock) {
      clock_tracker->SetGlobalClock(*trace_clock);
    }
    clock_tracker->AddDeferredClockSync(*trace_clock);
  }
  return base::OkStatus();
}

TraceManifestState::FileEntry* ForwardingTraceParser::FindManifestEntry()
    const {
  auto* state = input_context_->trace_manifest_state.get();
  if (state->files.empty()) {
    return nullptr;
  }
  auto row = input_context_->storage->trace_file_table()[file_id_];
  if (!row.name()) {
    return nullptr;
  }
  return state->FindEntry(
      input_context_->storage->GetString(*row.name()).ToStdString());
}

base::Status ForwardingTraceParser::Parse(TraceBlobView blob) {
  // If this is the first Parse() call, guess the trace type and create the
  // appropriate parser.
  if (!reader_) {
    RETURN_IF_ERROR(Init(blob));
  }
  trace_size_ += blob.size();
  return reader_->Parse(std::move(blob));
}

base::Status ForwardingTraceParser::OnPushDataToSorter() {
  if (reader_) {
    return reader_->OnPushDataToSorter();
  }
  return base::OkStatus();
}

void ForwardingTraceParser::OnEventsFullyExtracted() {
  if (reader_) {
    reader_->OnEventsFullyExtracted();
  }
  if (trace_type_ != kUnknownTraceType) {
    input_context_->trace_file_tracker->DoneParsing(file_id_, trace_size_);
  }
}

}  // namespace perfetto::trace_processor
