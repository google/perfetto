/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/perf/perf_data_parser.h"

#include <optional>
#include <string>
#include <vector>
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/perf/perf_data_reader.h"
#include "src/trace_processor/importers/perf/perf_data_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {

using FramesTable = tables::StackProfileFrameTable;
using CallsitesTable = tables::StackProfileCallsiteTable;

PerfDataParser::PerfDataParser(TraceProcessorContext* context)
    : context_(context), tracker_(PerfDataTracker::GetOrCreate(context_)) {}

PerfDataParser::~PerfDataParser() = default;

base::StatusOr<PerfDataTracker::PerfSample> PerfDataParser::ParseSample(
    TraceBlobView tbv) {
  perf_importer::Reader reader(std::move(tbv));
  return tracker_->ParseSample(reader);
}

void PerfDataParser::ParseTraceBlobView(int64_t ts, TraceBlobView tbv) {
  auto sample_status = ParseSample(std::move(tbv));
  if (!sample_status.ok()) {
    return;
  }
  PerfDataTracker::PerfSample sample = *sample_status;

  // The sample has been validated in tokenizer so callchain shouldn't be empty.
  PERFETTO_CHECK(!sample.callchain.empty());

  // First instruction pointer in the callchain should be from kernel space, so
  // it shouldn't be available in mappings.
  if (tracker_->FindMapping(*sample.pid, sample.callchain.front()).ok()) {
    context_->storage->IncrementStats(stats::perf_samples_skipped);
    return;
  }

  if (sample.callchain.size() == 1) {
    context_->storage->IncrementStats(stats::perf_samples_skipped);
    return;
  }

  std::vector<FramesTable::Row> frame_rows;
  for (uint32_t i = 1; i < sample.callchain.size(); i++) {
    auto mapping = tracker_->FindMapping(*sample.pid, sample.callchain[i]);
    if (!mapping.ok()) {
      context_->storage->IncrementStats(stats::perf_samples_skipped);
      return;
    }
    FramesTable::Row new_row;
    std::string mock_name =
        base::StackString<1024>("%" PRIu64,
                                sample.callchain[i] - mapping->start)
            .ToStdString();
    new_row.name = context_->storage->InternString(mock_name.c_str());
    new_row.mapping = mapping->id;
    new_row.rel_pc = static_cast<int64_t>(sample.callchain[i] - mapping->start);
    frame_rows.push_back(new_row);
  }

  // Insert frames. We couldn't do it before as no frames should be added if the
  // mapping couldn't be found for any of them.
  const auto& frames = context_->storage->mutable_stack_profile_frame_table();
  std::vector<FramesTable::Id> frame_ids;
  for (const auto& row : frame_rows) {
    frame_ids.push_back(frames->Insert(row).id);
  }

  // Insert callsites.
  const auto& callsites =
      context_->storage->mutable_stack_profile_callsite_table();

  std::optional<CallsitesTable::Id> parent_callsite_id;
  for (uint32_t i = 0; i < frame_ids.size(); i++) {
    CallsitesTable::Row callsite_row;
    callsite_row.frame_id = frame_ids[i];
    callsite_row.depth = i;
    callsite_row.parent_id = parent_callsite_id;
    parent_callsite_id = callsites->Insert(callsite_row).id;
  }

  // Insert stack sample.
  tables::PerfSampleTable::Row perf_sample_row;
  perf_sample_row.callsite_id = parent_callsite_id;
  perf_sample_row.ts = ts;
  if (sample.cpu) {
    perf_sample_row.cpu = *sample.cpu;
  }
  if (sample.tid) {
    auto utid = context_->process_tracker->GetOrCreateThread(*sample.tid);
    context_->process_tracker->GetOrCreateProcess(*sample.pid);
    perf_sample_row.utid = utid;
  }
  context_->storage->mutable_perf_sample_table()->Insert(perf_sample_row);
}

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto
