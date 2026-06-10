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
#include <string>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/trace_reader_registry.h"
#include "src/trace_processor/types/trace_metadata_state.h"
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
    case kPerfettoMetadataTraceType:
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

// Multiplier from a trace format's native timestamp unit to nanoseconds,
// used to scale perfetto_metadata anchor timestamps ("a ts as written in the
// file") into the clock graph's nanosecond domain.
int64_t FileUnitToNs(TraceType trace_type) {
  if (trace_type == kJsonTraceType) {
    return 1000;  // microseconds
  }
  if (trace_type == kGeckoTraceType) {
    return 1000000;  // milliseconds
  }
  return 1;
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

  if (file_id_.value == 0 && trace_type_ == kPerfettoMetadataTraceType) {
    return base::ErrStatus(
        "perfetto_metadata file must be inside an archive (zip/tar) "
        "containing the trace files it describes");
  }

  std::optional<TraceSorter::SortingMode> minimum_sorting_mode =
      GetMinimumSortingMode(trace_type_, *input_context_);
  if (minimum_sorting_mode) {
    input_context_->sorter->SetSortingMode(*minimum_sorting_mode);
  }
  input_context_->trace_file_tracker->StartParsing(file_id_, trace_type_);

  // If a perfetto_metadata file in the same archive has an entry for this
  // file (matched by exact path), it overrides clock/machine handling below.
  TraceMetadataState::FileEntry* metadata_entry = FindMetadataEntry();
  if (metadata_entry && IsContainerTraceType(trace_type_) &&
      (metadata_entry->machine_id || metadata_entry->clocks)) {
    return base::ErrStatus(
        "perfetto_metadata: overrides are not supported for archive members "
        "which are themselves archives: %s",
        metadata_entry->path.c_str());
  }

  if (IsContainerTraceType(trace_type_) ||
      trace_type_ == kPerfettoMetadataTraceType) {
    // perfetto_metadata files produce no events: like containers they do not
    // fork a per-trace context. This matters for clock composition: forking
    // would create the per-machine state and make this file the "primary"
    // trace for its machine, demoting the real traces (parsed later) to
    // non-primary and isolating their clock snapshots in private syncs.
    PERFETTO_DCHECK(!input_context_->trace_state);
    trace_context_ = input_context_;
  } else {
    uint32_t raw_machine_id = metadata_entry && metadata_entry->machine_id
                                  ? *metadata_entry->machine_id
                                  : 0;
    // TODO(b/334978369) Make sure kProtoTraceType and kSystraceTraceType are
    // parsed first so that we do not get issues with
    // SetPidZeroIsUpidZeroIdleProcess()
    trace_context_ =
        input_context_->ForkContextForTrace(file_id_, raw_machine_id);
    if (trace_type_ == kProtoTraceType || trace_type_ == kSystraceTraceType) {
      trace_context_->process_tracker->SetPidZeroIsUpidZeroIdleProcess();
    }
    if (metadata_entry) {
      trace_context_->trace_state->has_clock_override =
          metadata_entry->clocks.has_value();
      trace_context_->trace_state->has_machine_override =
          metadata_entry->machine_id.has_value();
    }
  }
  ASSIGN_OR_RETURN(reader_, input_context_->reader_registry->CreateTraceReader(
                                trace_type_, trace_context_));

  // Centralize clock setup for all trace formats. Every format declares the
  // clock domain its native timestamps are expressed in (its "trace clock"),
  // and we do two things with it:
  //
  //   1. Claim it as the global trace-time clock. The first trace to claim
  //      wins; later claims are silently ignored, so the global clock is
  //      stable regardless of how many traces an archive (e.g. a ZIP) holds.
  //
  //   2. Register a deferred identity sync for the same clock. If this trace
  //      does NOT win the global clock (e.g. a proto trace in the same archive
  //      claimed BOOTTIME first) and the trace clock is not otherwise linked
  //      into the clock graph via a ClockSnapshot, a zero-offset identity edge
  //      is injected on the first conversion. This keeps the trace's
  //      timestamps convertible instead of silently dropping every event. When
  //      a real ClockSnapshot does link the clock (e.g. proto provides
  //      BOOTTIME<->MONOTONIC), that real relationship is used instead.
  //
  // Proto traces are special: their clock is whatever ParseClockSnapshot reads
  // from primary_trace_clock, set later, so here we only register BOOTTIME as
  // the deferred fallback and do not claim the global clock now.
  //
  // A perfetto_metadata entry for this file can override the selection:
  // "native" reinterprets which clock the file's timestamps are on,
  // "offset_ns" shifts the file's events relative to where they would land
  // by default and "anchor" pins a file timestamp to a value on a named
  // clock domain.
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
             trace_type_ == kInstrumentsXmlTraceType) {
    trace_clock = ClockId::TraceFile(trace_context_->trace_id().value);
  } else if (trace_type_ == kAndroidDumpstateTraceType ||
             trace_type_ == kAndroidLogcatTraceType) {
    trace_clock = ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_REALTIME);
  }
  if (metadata_entry && metadata_entry->clocks) {
    const TraceMetadataState::ClocksOverride& clocks = *metadata_entry->clocks;
    if (clocks.native) {
      ClockId native_clock = ClockId::Machine(*clocks.native);
      if (trace_clock && *trace_clock != native_clock) {
        trace_context_->clock_tracker->SetNativeClockRemap(*trace_clock,
                                                           native_clock);
      }
      trace_clock = native_clock;
    }
    if (clocks.offset_ns) {
      trace_context_->clock_tracker->SetTraceTimeOffsetNs(*clocks.offset_ns);
    }
    if (clocks.anchor && trace_clock) {
      const TraceMetadataState::Anchor& anchor = *clocks.anchor;
      // The anchor source timestamp is expressed in the file's native units;
      // scale it into the nanosecond domain the clock graph operates in.
      int64_t src_ts_ns = static_cast<int64_t>(
          anchor.file_ts * static_cast<double>(FileUnitToNs(trace_type_)));
      // Inject the edge into the machine's shared clock graph (instead of
      // this file's ClockTracker) so it composes with snapshots from other
      // traces on the same machine (e.g. a proto trace's REALTIME<->BOOTTIME
      // snapshot). This is collision-safe: for the formats which use it the
      // source side is this file's unique TraceFile clock.
      RETURN_IF_ERROR(trace_context_->primary_clock_sync
                          ->AddSnapshot({{*trace_clock, src_ts_ns},
                                         {ClockId::Machine(anchor.target_clock),
                                          anchor.target_ts_ns}})
                          .status());
    }
    if (trace_type_ == kProtoTraceType && trace_clock) {
      // Proto packets without an explicit timestamp_clock_id skip clock
      // conversion entirely; route them through the trace default clock so
      // that offset/anchor overrides take effect.
      trace_context_->clock_tracker->SetTraceDefaultClock(*trace_clock);
    }
  }

  if (trace_clock) {
    if (claim_global_clock) {
      trace_context_->clock_tracker->SetGlobalClock(*trace_clock);
    }
    trace_context_->clock_tracker->AddDeferredIdentitySync(*trace_clock);
  }
  return base::OkStatus();
}

TraceMetadataState::FileEntry* ForwardingTraceParser::FindMetadataEntry()
    const {
  auto* state = input_context_->trace_metadata_state.get();
  if (!state || state->files.empty()) {
    return nullptr;
  }
  auto row = input_context_->storage->trace_file_table()[file_id_];
  if (!row.name()) {
    return nullptr;
  }
  std::string name =
      input_context_->storage->GetString(*row.name()).ToStdString();
  TraceMetadataState::FileEntry* entry = state->FindEntry(name);
  if (entry) {
    entry->matched = true;
  }
  return entry;
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
