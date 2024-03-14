/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/jit_tracker.h"

#include <cstdint>
#include <optional>
#include <string>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/address_range.h"
#include "src/trace_processor/importers/common/jit_cache.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/jit_tables_py.h"
#include "src/trace_processor/util/build_id.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::FieldsAre;
using ::testing::IsEmpty;
using ::testing::Ne;
using ::testing::Optional;
using ::testing::SaveArg;

class JitTrackerTest : public testing::Test {
 public:
  JitTrackerTest() {
    context_.storage.reset(new TraceStorage());
    context_.stack_profile_tracker.reset(new StackProfileTracker(&context_));
    context_.mapping_tracker.reset(new MappingTracker(&context_));
    context_.process_tracker.reset(new ProcessTracker(&context_));
    jit_tracker_ = JitTracker::GetOrCreate(&context_);
  }

 protected:
  UserMemoryMapping& AddMapping(UniquePid upid,
                                AddressRange range,
                                uint64_t exact_offset = 0,
                                uint64_t load_bias = 0) {
    uint32_t id = context_.storage->stack_profile_mapping_table().row_count();
    CreateMappingParams params;
    params.memory_range = range;
    params.build_id =
        BuildId::FromRaw(reinterpret_cast<const uint8_t*>(&id), sizeof(id));
    params.exact_offset = exact_offset;
    params.start_offset = exact_offset;
    params.load_bias = load_bias;
    params.name = "Mapping ";
    params.name += std::to_string(id);
    return context_.mapping_tracker->CreateUserMemoryMapping(upid,
                                                             std::move(params));
  }

  TraceProcessorContext context_;
  JitTracker* jit_tracker_;
};

TEST_F(JitTrackerTest, BasicFunctionality) {
  const UniquePid upid = context_.process_tracker->GetOrCreateProcess(1234);
  const UniqueTid utid = context_.process_tracker->UpdateThread(4321, 1234);
  const AddressRange jit_range(0, 1000);
  auto& mapping = AddMapping(upid, jit_range);
  JitCache* cache = jit_tracker_->CreateJitCache("name", upid, jit_range);

  const StringId function_name = context_.storage->InternString("Function 1");
  const StringId source_file = context_.storage->InternString("SourceFile");
  const int64_t create_ts = 12345;
  const AddressRange code_range(0, 100);

  auto code_id = cache->LoadCode(create_ts, utid, code_range, function_name,
                                 JitCache::SourceLocation{source_file, 10},
                                 TraceBlobView());

  auto code = *context_.storage->jit_code_table().FindById(code_id);
  EXPECT_THAT(code.create_ts(), Eq(create_ts));
  EXPECT_THAT(code.estimated_delete_ts(), Eq(std::nullopt));
  EXPECT_THAT(code.utid(), Eq(utid));
  EXPECT_THAT(code.start_address(),
              Eq(static_cast<int64_t>(code_range.start())));
  EXPECT_THAT(code.size(), Eq(static_cast<int64_t>(code_range.size())));
  EXPECT_THAT(code.function_name(), Eq(function_name));

  auto frame_id = mapping.InternFrame(50, "");

  auto frame =
      *context_.storage->stack_profile_frame_table().FindById(frame_id);
  EXPECT_THAT(frame.name(), Eq(function_name));

  auto row = context_.storage->jit_frame_table().FindById(
      tables::JitFrameTable::Id(0));
  ASSERT_THAT(row, Ne(std::nullopt));

  EXPECT_THAT(row->jit_code_id(), Eq(code_id));
  EXPECT_THAT(row->frame_id(), Eq(frame_id));
}

TEST_F(JitTrackerTest, FunctionOverlapUpdatesDeleteTs) {
  const UniquePid upid = context_.process_tracker->GetOrCreateProcess(1234);
  const UniqueTid utid = context_.process_tracker->UpdateThread(4321, 1234);
  const AddressRange jit_range(0, 1000);
  auto& mapping = AddMapping(upid, jit_range);
  JitCache* cache = jit_tracker_->CreateJitCache("name", upid, jit_range);

  const StringId function_name_1 = context_.storage->InternString("Function 1");
  const StringId function_name_2 = context_.storage->InternString("Function 2");
  const StringId source_file = context_.storage->InternString("SourceFile");
  const int64_t create_ts_1 = 12345;
  const int64_t create_ts_2 = 23456;
  const AddressRange code_range_1(0, 100);
  const AddressRange code_range_2(50, 200);

  auto code_id_1 = cache->LoadCode(
      create_ts_1, utid, code_range_1, function_name_1,
      JitCache::SourceLocation{source_file, 10}, TraceBlobView());
  auto code_id_2 = cache->LoadCode(
      create_ts_2, utid, code_range_2, function_name_2,
      JitCache::SourceLocation{source_file, 10}, TraceBlobView());
  EXPECT_THAT(code_id_1, Ne(code_id_2));

  auto code_1 = *context_.storage->jit_code_table().FindById(code_id_1);
  auto code_2 = *context_.storage->jit_code_table().FindById(code_id_2);

  // Code 1 has been deleted
  EXPECT_THAT(code_1.create_ts(), Eq(create_ts_1));
  EXPECT_THAT(code_1.estimated_delete_ts(), Eq(create_ts_2));

  // The only active code is 2 at this point.
  EXPECT_THAT(code_2.create_ts(), Eq(create_ts_2));
  EXPECT_THAT(code_2.estimated_delete_ts(), Eq(std::nullopt));

  // No frame should mention code 1
  FrameId frame_id = mapping.InternFrame(50, "");
  auto frame_a =
      *context_.storage->stack_profile_frame_table().FindById(frame_id);
  EXPECT_THAT(frame_a.name(), Eq(function_name_2));
  ASSERT_THAT(context_.storage->jit_frame_table().row_count(), Eq(1u));
  auto row = context_.storage->jit_frame_table().FindById(
      tables::JitFrameTable::Id(0));
  EXPECT_THAT(row->jit_code_id(), Eq(code_id_2));
  EXPECT_THAT(row->frame_id(), Eq(frame_id));

  // Frames for the old code 1 must fail to resolve to a jitted function but
  // still generate a frame.
  EXPECT_THAT(context_.storage->stats().at(stats::jit_unknown_frame).value,
              Eq(0));
  frame_id = mapping.InternFrame(0, "custom");
  EXPECT_THAT(context_.storage->stats().at(stats::jit_unknown_frame).value,
              Eq(1));
  auto frame_b =
      *context_.storage->stack_profile_frame_table().FindById(frame_id);
  EXPECT_THAT(frame_a.id(), Ne(frame_b.id()));
  EXPECT_THAT(context_.storage->GetString(frame_b.name()), Eq("custom"));
  EXPECT_THAT(context_.storage->jit_frame_table().row_count(), Eq(1u));
}

}  // namespace

}  // namespace trace_processor
}  // namespace perfetto
