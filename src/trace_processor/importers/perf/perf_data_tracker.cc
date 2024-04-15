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

#include "src/trace_processor/importers/perf/perf_data_tracker.h"

#include <optional>

#include "perfetto/base/status.h"
#include "src/trace_processor/importers/common/address_range.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {
namespace {

bool IsInKernel(protos::pbzero::Profiling::CpuMode cpu_mode) {
  switch (cpu_mode) {
    case protos::pbzero::Profiling::MODE_UNKNOWN:
      PERFETTO_CHECK(false);
    case protos::pbzero::Profiling::MODE_GUEST_KERNEL:
    case protos::pbzero::Profiling::MODE_KERNEL:
      return true;
    case protos::pbzero::Profiling::MODE_USER:
    case protos::pbzero::Profiling::MODE_HYPERVISOR:
    case protos::pbzero::Profiling::MODE_GUEST_USER:
      return false;
  }
  PERFETTO_CHECK(false);
}

CreateMappingParams BuildCreateMappingParams(
    PerfDataTracker::Mmap2Record record) {
  return {AddressRange::FromStartAndSize(record.num.addr, record.num.len),
          record.num.pgoff,
          // start_offset: This is the offset into the file where the ELF header
          // starts. We assume all file mappings are ELF files an thus this
          // offset is 0.
          0,
          // load_bias: This can only be read out of the actual ELF file, which
          // we do not have here, so we set it to 0. When symbolizing we will
          // hopefully have the real load bias and we can compensate there for a
          // possible mismatch.
          0, record.filename, std::nullopt};
}
}  // namespace

PerfDataTracker::~PerfDataTracker() = default;

uint64_t PerfDataTracker::ComputeCommonSampleType() {
  if (attrs_.empty()) {
    return 0;
  }
  common_sample_type_ = std::numeric_limits<uint64_t>::max();
  for (const auto& a : attrs_) {
    common_sample_type_ &= a.attr.sample_type;
  }
  return common_sample_type_;
}

const perf_event_attr* PerfDataTracker::FindAttrWithId(uint64_t id) const {
  for (const auto& attr_and_ids : attrs_) {
    if (auto x =
            std::find(attr_and_ids.ids.begin(), attr_and_ids.ids.end(), id);
        x == attr_and_ids.ids.end()) {
      continue;
    }
    return &attr_and_ids.attr;
  }
  return nullptr;
}

void PerfDataTracker::PushMmap2Record(Mmap2Record record) {
  if (IsInKernel(record.cpu_mode)) {
    context_->mapping_tracker->CreateKernelMemoryMapping(
        BuildCreateMappingParams(std::move(record)));
  } else {
    UniquePid upid =
        context_->process_tracker->GetOrCreateProcess(record.num.pid);
    context_->mapping_tracker->CreateUserMemoryMapping(
        upid, BuildCreateMappingParams(std::move(record)));
  }
}

base::StatusOr<PerfDataTracker::PerfSample> PerfDataTracker::ParseSample(
    perfetto::trace_processor::perf_importer::PerfDataReader& reader) {
  uint64_t sample_type = common_sample_type();
  PerfDataTracker::PerfSample sample;

  if (sample_type & PERF_SAMPLE_IDENTIFIER) {
    reader.ReadOptional(sample.id);
    if (auto attr = FindAttrWithId(*sample.id); attr) {
      sample_type = attr->sample_type;
    } else {
      return base::ErrStatus("No attr for sample_id");
    }
  }

  if (sample_type & PERF_SAMPLE_IP) {
    reader.Skip<uint64_t>();
  }

  if (sample_type & PERF_SAMPLE_TID) {
    reader.ReadOptional(sample.pid);
    reader.ReadOptional(sample.tid);
  }

  if (sample_type & PERF_SAMPLE_TIME) {
    reader.ReadOptional(sample.ts);
  }

  // Ignored. Checked because we need to access later parts of sample.
  if (sample_type & PERF_SAMPLE_ADDR) {
    reader.Skip<uint64_t>();
  }

  // The same value as PERF_SAMPLE_IDENTIFIER, so should be ignored.
  if (sample_type & PERF_SAMPLE_ID) {
    reader.Skip<uint64_t>();
  }

  // Ignored. Checked because we need to access later parts of sample.
  if (sample_type & PERF_SAMPLE_STREAM_ID) {
    reader.Skip<uint64_t>();
  }

  if (sample_type & PERF_SAMPLE_CPU) {
    reader.ReadOptional(sample.cpu);
    // Ignore next uint32_t res.
    reader.Skip<uint32_t>();
  }

  // Ignored. Checked because we need to access later parts of sample.
  if (sample_type & PERF_SAMPLE_PERIOD) {
    reader.Skip<uint64_t>();
  }

  // Ignored.
  // TODO(mayzner): Implement.
  if (sample_type & PERF_SAMPLE_READ) {
    context_->storage->IncrementStats(stats::perf_samples_skipped);
    return base::ErrStatus("PERF_SAMPLE_READ is not supported");
  }

  if (sample_type & PERF_SAMPLE_CALLCHAIN) {
    uint64_t vec_size;
    reader.Read(vec_size);

    sample.callchain.resize(static_cast<size_t>(vec_size));
    reader.ReadVector(sample.callchain);
  }

  return sample;
}

PerfDataTracker* PerfDataTracker::GetOrCreate(TraceProcessorContext* context) {
  if (!context->perf_data_tracker) {
    context->perf_data_tracker.reset(new PerfDataTracker(context));
  }
  return static_cast<PerfDataTracker*>(context->perf_data_tracker.get());
}
}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto
