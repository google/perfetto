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

#include <stddef.h>
#include <cstring>
#include <memory>
#include <vector>

#include "perfetto/base/build_config.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "src/trace_processor/importers/common/address_range.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/perf/perf_event.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {
namespace {

class PerfDataTrackerUnittest : public testing::Test {
 public:
  PerfDataTrackerUnittest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.process_tracker = std::make_unique<ProcessTracker>(&context_);
    context_.stack_profile_tracker =
        std::make_unique<StackProfileTracker>(&context_);
    context_.mapping_tracker = std::make_unique<MappingTracker>(&context_);
  }

 protected:
  TraceProcessorContext context_;
};

TEST_F(PerfDataTrackerUnittest, ComputeCommonSampleType) {
  PerfDataTracker* tracker = PerfDataTracker::GetOrCreate(&context_);

  PerfDataTracker::AttrAndIds attr_and_ids;
  attr_and_ids.attr.sample_type =
      PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME;
  tracker->PushAttrAndIds(attr_and_ids);

  attr_and_ids.attr.sample_type = PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU;
  tracker->PushAttrAndIds(attr_and_ids);

  tracker->ComputeCommonSampleType();
  EXPECT_TRUE(tracker->common_sample_type() & PERF_SAMPLE_CPU);
  EXPECT_FALSE(tracker->common_sample_type() & PERF_SAMPLE_CALLCHAIN);
}

TEST_F(PerfDataTrackerUnittest, FindMapping) {
  PerfDataTracker* tracker = PerfDataTracker::GetOrCreate(&context_);

  PerfDataTracker::Mmap2Record rec;
  rec.cpu_mode = protos::pbzero::Profiling::MODE_USER;
  rec.filename = "file1";
  rec.num.addr = 1000;
  rec.num.len = 100;
  rec.num.pid = 1;
  rec.cpu_mode = protos::pbzero::Profiling::MODE_USER;
  tracker->PushMmap2Record(rec);

  rec.num.addr = 2000;
  tracker->PushMmap2Record(rec);

  rec.num.addr = 3000;
  tracker->PushMmap2Record(rec);

  UserMemoryMapping* mapping =
      context_.mapping_tracker->FindUserMappingForAddress(
          context_.process_tracker->GetOrCreateProcess(1), 2050);
  ASSERT_NE(mapping, nullptr);
  EXPECT_EQ(mapping->memory_range().start(), 2000u);
  EXPECT_EQ(mapping->memory_range().end(), 2100u);
}

TEST_F(PerfDataTrackerUnittest, FindMappingFalse) {
  PerfDataTracker* tracker = PerfDataTracker::GetOrCreate(&context_);

  PerfDataTracker::Mmap2Record rec;
  rec.cpu_mode = protos::pbzero::Profiling::MODE_USER;
  rec.filename = "file1";
  rec.num.addr = 1000;
  rec.num.len = 100;
  rec.num.pid = 1;
  rec.cpu_mode = protos::pbzero::Profiling::MODE_USER;
  tracker->PushMmap2Record(rec);

  UserMemoryMapping* mapping =
      context_.mapping_tracker->FindUserMappingForAddress(
          context_.process_tracker->GetOrCreateProcess(2), 2050);
  EXPECT_EQ(mapping, nullptr);
}

TEST_F(PerfDataTrackerUnittest, ParseSampleTrivial) {
  PerfDataTracker* tracker = PerfDataTracker::GetOrCreate(&context_);

  PerfDataTracker::AttrAndIds attr_and_ids;
  attr_and_ids.attr.sample_type = PERF_SAMPLE_TIME;
  tracker->PushAttrAndIds(attr_and_ids);
  tracker->ComputeCommonSampleType();

  uint64_t ts = 100;

  TraceBlob blob =
      TraceBlob::CopyFrom(static_cast<const void*>(&ts), sizeof(uint64_t));
  PerfDataReader reader(TraceBlobView(std::move(blob)));

  auto parsed_sample = tracker->ParseSample(reader);
  EXPECT_TRUE(parsed_sample.ok());
  EXPECT_EQ(parsed_sample->ts, 100u);
}

TEST_F(PerfDataTrackerUnittest, ParseSampleCallchain) {
  PerfDataTracker* tracker = PerfDataTracker::GetOrCreate(&context_);

  PerfDataTracker::AttrAndIds attr_and_ids;
  attr_and_ids.attr.sample_type = PERF_SAMPLE_CALLCHAIN;
  tracker->PushAttrAndIds(attr_and_ids);
  tracker->ComputeCommonSampleType();

  struct Sample {
    uint64_t callchain_size;         /* if PERF_SAMPLE_CALLCHAIN */
    std::vector<uint64_t> callchain; /* if PERF_SAMPLE_CALLCHAIN */
  };

  Sample sample;
  sample.callchain_size = 3;
  sample.callchain = std::vector<uint64_t>{1, 2, 3};

  TraceBlob blob = TraceBlob::Allocate(4 * sizeof(uint64_t));
  memcpy(blob.data(), &sample.callchain_size, sizeof(uint64_t));
  memcpy(blob.data() + sizeof(uint64_t), sample.callchain.data(),
         sizeof(uint64_t) * 3);
  PerfDataReader reader(TraceBlobView(std::move(blob)));

  auto parsed_sample = tracker->ParseSample(reader);
  EXPECT_TRUE(parsed_sample.ok());
  EXPECT_EQ(parsed_sample->callchain.size(), 3u);
}

TEST_F(PerfDataTrackerUnittest, ParseSampleWithoutId) {
  PerfDataTracker* tracker = PerfDataTracker::GetOrCreate(&context_);

  PerfDataTracker::AttrAndIds attr_and_ids;
  attr_and_ids.attr.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
                                  PERF_SAMPLE_CPU | PERF_SAMPLE_CALLCHAIN;
  tracker->PushAttrAndIds(attr_and_ids);
  tracker->ComputeCommonSampleType();

  struct Sample {
    uint32_t pid;            /* if PERF_SAMPLE_TID */
    uint32_t tid;            /* if PERF_SAMPLE_TID */
    uint64_t ts;             /* if PERF_SAMPLE_TIME */
    uint32_t cpu;            /* if PERF_SAMPLE_CPU */
    uint32_t res_ignore;     /* if PERF_SAMPLE_CPU */
    uint64_t callchain_size; /* if PERF_SAMPLE_CALLCHAIN */
  };

  Sample sample;
  sample.pid = 2;
  sample.ts = 100;
  sample.cpu = 1;
  sample.callchain_size = 3;
  std::vector<uint64_t> callchain{1, 2, 3};

  TraceBlob blob = TraceBlob::Allocate(sizeof(Sample) + sizeof(uint64_t) * 3);
  memcpy(blob.data(), &sample, sizeof(Sample));
  memcpy(blob.data() + sizeof(Sample), callchain.data(), sizeof(uint64_t) * 3);

  PerfDataReader reader(TraceBlobView(std::move(blob)));
  EXPECT_TRUE(reader.CanReadSize(sizeof(Sample)));

  auto parsed_sample = tracker->ParseSample(reader);
  EXPECT_TRUE(parsed_sample.ok());
  EXPECT_EQ(parsed_sample->callchain.size(), 3u);
  EXPECT_EQ(sample.ts, parsed_sample->ts);
}

TEST_F(PerfDataTrackerUnittest, ParseSampleWithId) {
  PerfDataTracker* tracker = PerfDataTracker::GetOrCreate(&context_);

  PerfDataTracker::AttrAndIds attr_and_ids;
  attr_and_ids.attr.sample_type = PERF_SAMPLE_CPU | PERF_SAMPLE_TID |
                                  PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_ID |
                                  PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_TIME;
  attr_and_ids.ids.push_back(10);
  tracker->PushAttrAndIds(attr_and_ids);
  tracker->ComputeCommonSampleType();

  struct Sample {
    uint64_t identifier;     /* if PERF_SAMPLE_IDENTIFIER */
    uint32_t pid;            /* if PERF_SAMPLE_TID */
    uint32_t tid;            /* if PERF_SAMPLE_TID */
    uint64_t ts;             /* if PERF_SAMPLE_TIME */
    uint64_t id;             /* if PERF_SAMPLE_ID */
    uint32_t cpu;            /* if PERF_SAMPLE_CPU */
    uint32_t res_ignore;     /* if PERF_SAMPLE_CPU */
    uint64_t callchain_size; /* if PERF_SAMPLE_CALLCHAIN */
  };

  Sample sample;
  sample.id = 10;
  sample.identifier = 10;
  sample.cpu = 1;
  sample.pid = 2;
  sample.ts = 100;
  sample.callchain_size = 3;
  std::vector<uint64_t> callchain{1, 2, 3};

  TraceBlob blob = TraceBlob::Allocate(sizeof(Sample) + sizeof(uint64_t) * 3);
  memcpy(blob.data(), &sample, sizeof(Sample));
  memcpy(blob.data() + sizeof(Sample), callchain.data(), sizeof(uint64_t) * 3);

  PerfDataReader reader(TraceBlobView(std::move(blob)));

  auto parsed_sample = tracker->ParseSample(reader);
  EXPECT_TRUE(parsed_sample.ok());
  EXPECT_EQ(parsed_sample->callchain.size(), 3u);
  EXPECT_EQ(100u, parsed_sample->ts);
}

}  // namespace
}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto
