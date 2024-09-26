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

#include "src/trace_processor/importers/perf/aux_stream_manager.h"
#include <cstdint>
#include <memory>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/aux_record.h"
#include "src/trace_processor/importers/perf/auxtrace_info_record.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::perf_importer {
namespace {

std::unique_ptr<TraceProcessorContext> CreateTraceProcessorContext() {
  auto ctx = std::make_unique<TraceProcessorContext>();
  ctx->storage = std::make_shared<TraceStorage>();
  return ctx;
}

AuxtraceInfoRecord CreateAuxtraceInfoRecord() {
  AuxtraceInfoRecord info;
  info.type = 0;
  return info;
}

AuxRecord CreateAuxRecord(uint64_t offset, uint64_t size, uint32_t cpu) {
  AuxRecord aux;
  aux.offset = offset;
  aux.size = size;
  aux.flags = 0;
  aux.sample_id.emplace();
  aux.sample_id->set_cpu(cpu);
  return aux;
}

AuxtraceRecord CreateAuxtraceRecord(uint64_t offset,
                                    uint64_t size,
                                    uint32_t cpu) {
  AuxtraceRecord auxtrace;
  auxtrace.offset = offset;
  auxtrace.size = size;
  auxtrace.cpu = cpu;
  auxtrace.tid = 0;
  return auxtrace;
}

TEST(AuxStreamManagerTest, NoAuxStreamsCanFinalize) {
  auto ctx = CreateTraceProcessorContext();
  AuxStreamManager manager(ctx.get());
  EXPECT_TRUE(manager.FinalizeStreams().ok());
}

TEST(AuxStreamManagerTest, NoAuxTraceInfoFailsMethods) {
  auto ctx = CreateTraceProcessorContext();
  AuxStreamManager manager(ctx.get());

  EXPECT_FALSE(manager
                   .OnAuxtraceRecord(CreateAuxtraceRecord(0, 10, 0),
                                     TraceBlobView(TraceBlob::Allocate(10)))
                   .ok());
  EXPECT_FALSE(manager.OnAuxRecord(CreateAuxRecord(0, 10, 0)).ok());
}

TEST(AuxStreamManagerTest, MultipleAuxTraceInfoFails) {
  auto ctx = CreateTraceProcessorContext();
  AuxStreamManager manager(ctx.get());

  AuxtraceInfoRecord info_0;
  info_0.type = 0;
  EXPECT_TRUE(manager.OnAuxtraceInfoRecord(std::move(info_0)).ok());

  AuxtraceInfoRecord info_1;
  info_1.type = 1;
  EXPECT_FALSE(manager.OnAuxtraceInfoRecord(std::move(info_1)).ok());
}

TEST(AuxStreamManagerTest, ReconstructsStream) {
  constexpr uint64_t kSize = 10;
  constexpr uint32_t kCpu = 0;
  TraceBlobView data(TraceBlob::Allocate(kSize));
  TraceBlobView double_data(TraceBlob::Allocate(2 * kSize));
  auto ctx = CreateTraceProcessorContext();
  AuxStreamManager manager(ctx.get());
  ASSERT_TRUE(manager.OnAuxtraceInfoRecord(CreateAuxtraceInfoRecord()).ok());

  manager.OnAuxRecord(CreateAuxRecord(0, kSize, kCpu));
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 0);

  manager.OnAuxRecord(CreateAuxRecord(10, kSize, kCpu));
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 0);

  manager.OnAuxtraceRecord(CreateAuxtraceRecord(0, 2 * kSize, kCpu),
                           double_data.copy());
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 20);

  manager.OnAuxtraceRecord(CreateAuxtraceRecord(20, kSize, kCpu), data.copy());
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 20);

  manager.OnAuxtraceRecord(CreateAuxtraceRecord(30, kSize, kCpu), data.copy());
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 20);

  manager.OnAuxRecord(CreateAuxRecord(20, 2 * kSize, kCpu));
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 40);
}

TEST(AuxStreamManagerTest, AuxLoss) {
  constexpr uint64_t kSize = 10;
  constexpr uint32_t kCpu = 0;
  TraceBlobView data(TraceBlob::Allocate(kSize));
  TraceBlobView triple_data(TraceBlob::Allocate(3 * kSize));
  auto ctx = CreateTraceProcessorContext();
  AuxStreamManager manager(ctx.get());
  ASSERT_TRUE(manager.OnAuxtraceInfoRecord(CreateAuxtraceInfoRecord()).ok());

  manager.OnAuxRecord(CreateAuxRecord(10, kSize, kCpu));
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_missing].value, 10);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_auxtrace_missing].value, 0);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 0);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_lost].value, 0);

  manager.OnAuxtraceRecord(CreateAuxtraceRecord(0, 3 * kSize, kCpu),
                           triple_data.copy());
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_missing].value, 10);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_auxtrace_missing].value, 0);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 10);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_lost].value, 10);

  manager.FinalizeStreams();
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_missing].value, 20);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_auxtrace_missing].value, 0);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 10);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_lost].value, 20);
}

TEST(AuxStreamManagerTest, AuxtraceLoss) {
  constexpr uint64_t kSize = 10;
  constexpr uint32_t kCpu = 0;
  TraceBlobView data(TraceBlob::Allocate(kSize));
  auto ctx = CreateTraceProcessorContext();
  AuxStreamManager manager(ctx.get());
  ASSERT_TRUE(manager.OnAuxtraceInfoRecord(CreateAuxtraceInfoRecord()).ok());

  manager.OnAuxtraceRecord(CreateAuxtraceRecord(10, kSize, kCpu), data.copy());
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_missing].value, 0);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_auxtrace_missing].value, 10);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 0);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_lost].value, 0);

  manager.OnAuxRecord(CreateAuxRecord(0, 3 * kSize, kCpu));
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_missing].value, 0);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_auxtrace_missing].value, 10);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 10);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_lost].value, 10);

  manager.FinalizeStreams();
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_missing].value, 0);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_auxtrace_missing].value, 20);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 10);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_lost].value, 20);
}

TEST(AuxStreamManagerTest, ComplexStream) {
  constexpr uint32_t kCpu = 0;
  TraceBlobView data_5(TraceBlob::Allocate(5));
  TraceBlobView data_10(TraceBlob::Allocate(10));
  TraceBlobView data_15(TraceBlob::Allocate(15));

  auto ctx = CreateTraceProcessorContext();
  AuxStreamManager manager(ctx.get());
  ASSERT_TRUE(manager.OnAuxtraceInfoRecord(CreateAuxtraceInfoRecord()).ok());

  uint64_t aux_offset = 0;
  uint64_t auxtrace_offset = 0;

  auto aux = [&](uint64_t size) {
    manager.OnAuxRecord(CreateAuxRecord(aux_offset, size, kCpu));
    aux_offset += size;
  };
  auto aux_hole = [&](uint64_t size) { aux_offset += size; };

  auto auxtrace = [&](uint64_t size) {
    manager.OnAuxtraceRecord(CreateAuxtraceRecord(auxtrace_offset, size, kCpu),
                             TraceBlobView(TraceBlob::Allocate(size)));
    auxtrace_offset += size;
  };
  auto auxtrace_hole = [&](uint64_t size) { auxtrace_offset += size; };

  //          . . . . . . . . . . . . . . . . . . . . . .
  //          |105                                      |
  // Aux      |---|30         |10 |30         |-|20     |
  // Auxtrace |5|10 |50                 |5|-|5|---|5|---|
  // Result   |---|60                     |-|5|---|5|---|
  //          . . . . . . . . . . . . . . . . . . . . . .
  aux_hole(10);
  aux(30);
  aux(10);
  aux(30);
  aux_hole(5);
  aux(20);
  auxtrace(5);
  auxtrace(10);
  auxtrace(50);
  auxtrace(5);
  auxtrace_hole(5);
  auxtrace(5);
  auxtrace_hole(10);
  auxtrace(5);

  manager.FinalizeStreams();

  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_missing].value, 15);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_auxtrace_missing].value, 25);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 70);
  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_lost].value, 35);
}

TEST(AuxStreamManagerTest, StreamOverlapFails) {
  constexpr uint64_t kSize = 10;
  constexpr uint32_t kCpu = 0;
  TraceBlobView data(TraceBlob::Allocate(kSize));
  auto ctx = CreateTraceProcessorContext();
  AuxStreamManager manager(ctx.get());
  ASSERT_TRUE(manager.OnAuxtraceInfoRecord(CreateAuxtraceInfoRecord()).ok());

  EXPECT_TRUE(manager.OnAuxRecord(CreateAuxRecord(0, kSize, kCpu)).ok());
  EXPECT_FALSE(manager.OnAuxRecord(CreateAuxRecord(0, kSize, kCpu)).ok());

  EXPECT_TRUE(
      manager
          .OnAuxtraceRecord(CreateAuxtraceRecord(0, kSize, kCpu), data.copy())
          .ok());
  EXPECT_FALSE(
      manager
          .OnAuxtraceRecord(CreateAuxtraceRecord(0, kSize, kCpu), data.copy())
          .ok());
}

TEST(AuxStreamManagerTest, MultipleStreams) {
  constexpr uint64_t kSize = 10;
  constexpr uint32_t kCpu_0 = 0;
  constexpr uint32_t kCpu_1 = 1;
  TraceBlobView data(TraceBlob::Allocate(kSize));
  auto ctx = CreateTraceProcessorContext();
  AuxStreamManager manager(ctx.get());
  ASSERT_TRUE(manager.OnAuxtraceInfoRecord(CreateAuxtraceInfoRecord()).ok());

  EXPECT_TRUE(manager.OnAuxRecord(CreateAuxRecord(0, kSize, kCpu_0)).ok());
  EXPECT_TRUE(manager.OnAuxRecord(CreateAuxRecord(0, kSize, kCpu_1)).ok());

  EXPECT_TRUE(
      manager
          .OnAuxtraceRecord(CreateAuxtraceRecord(0, kSize, kCpu_0), data.copy())
          .ok());
  EXPECT_TRUE(
      manager
          .OnAuxtraceRecord(CreateAuxtraceRecord(0, kSize, kCpu_1), data.copy())
          .ok());

  manager.FinalizeStreams();

  EXPECT_EQ(ctx->storage->stats()[stats::perf_aux_ignored].value, 20);
}

}  // namespace
}  // namespace perfetto::trace_processor::perf_importer
