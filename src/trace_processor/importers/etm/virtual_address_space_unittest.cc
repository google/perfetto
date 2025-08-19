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

#include "src/trace_processor/importers/etm/virtual_address_space.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "src/trace_processor/importers/etm/mapping_version.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/perf_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::etm {
namespace {
using ::testing::IsNull;
using ::testing::Pointee;
using ::testing::Property;

auto MappingIdIs(tables::MmapRecordTable::ConstRowReference mmap) {
  return Pointee(
      Property("mapping_id", &MappingVersion::id, mmap.mapping_id()));
}

tables::MmapRecordTable::ConstRowReference AddMapping(
    TraceStorage& storage,
    int64_t ts,
    std::optional<UniquePid> upid,
    uint64_t start,
    uint64_t end) {
  auto mapping_id =
      storage.mutable_stack_profile_mapping_table()
          ->Insert({kNullStringId, 0, 0, static_cast<int64_t>(start),
                    static_cast<int64_t>(end)})
          .id;
  return storage.mutable_mmap_record_table()
      ->Insert({ts, upid, mapping_id})
      .row_reference;
}

TEST(VirtualAddressSpaceTest, Empty) {
  TraceProcessorContext context;
  context.storage = std::make_unique<TraceStorage>();
  VirtualAddressSpace vs = VirtualAddressSpace::Builder(&context).Build();

  EXPECT_THAT(vs.FindMapping(0, 5), IsNull());
}

TEST(VirtualAddressSpaceTest, DisjointRanges) {
  TraceProcessorContext context;
  context.storage = std::make_unique<TraceStorage>();
  auto builder = VirtualAddressSpace::Builder(&context);
  const UniquePid upid = 123;

  auto m_1 = AddMapping(*context.storage, 10, upid, 10, 100);
  builder.AddMapping(m_1);
  auto m_2 = AddMapping(*context.storage, 10, upid, 200, 300);
  builder.AddMapping(m_2);
  VirtualAddressSpace vs = std::move(builder).Build();

  EXPECT_THAT(vs.FindMapping(0, 10), IsNull());
  EXPECT_THAT(vs.FindMapping(9, 10), IsNull());
  EXPECT_THAT(vs.FindMapping(10, 9), IsNull());
  EXPECT_THAT(vs.FindMapping(10, 10), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(10, 99), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(10, 100), IsNull());

  EXPECT_THAT(vs.FindMapping(10, 199), IsNull());
  EXPECT_THAT(vs.FindMapping(10, 200), MappingIdIs(m_2));
}

TEST(VirtualAddressSpaceTest, ComplexLayout) {
  TraceProcessorContext context;
  context.storage = std::make_unique<TraceStorage>();
  auto builder = VirtualAddressSpace::Builder(&context);
  const UniquePid upid = 123;

  auto m_1 = AddMapping(*context.storage, 10, upid, 10, 100);
  builder.AddMapping(m_1);
  auto m_2 = AddMapping(*context.storage, 20, upid, 20, 80);
  builder.AddMapping(m_2);
  auto m_3 = AddMapping(*context.storage, 30, upid, 5, 50);
  builder.AddMapping(m_3);
  auto m_4 = AddMapping(*context.storage, 40, upid, 70, 200);
  builder.AddMapping(m_4);
  VirtualAddressSpace vs = std::move(builder).Build();
  //  T  ^
  //  i  |
  //  m  |
  //  e  |
  // 40  |                     <70----------------------200>
  //     |
  // 30  |  <5------------50>
  //     |
  // 20  |        <20-----------80>
  //     |
  // 10  |    <10-------------------100>
  //     |--------------------------------------------------> address

  EXPECT_THAT(vs.FindMapping(0, 5), IsNull());
  EXPECT_THAT(vs.FindMapping(9, 50), IsNull());
  EXPECT_THAT(vs.FindMapping(30, 100), IsNull());
  EXPECT_THAT(vs.FindMapping(39, 180), IsNull());
  EXPECT_THAT(vs.FindMapping(19, 10), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(19, 20), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(19, 50), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(20, 50), MappingIdIs(m_2));
  EXPECT_THAT(vs.FindMapping(29, 10), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(29, 19), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(29, 80), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(29, 99), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(30, 50), MappingIdIs(m_2));
  EXPECT_THAT(vs.FindMapping(30, 80), MappingIdIs(m_1));
  EXPECT_THAT(vs.FindMapping(30, 100), IsNull());
  EXPECT_THAT(vs.FindMapping(40, 5), MappingIdIs(m_3));
  EXPECT_THAT(vs.FindMapping(40, 10), MappingIdIs(m_3));
  EXPECT_THAT(vs.FindMapping(40, 20), MappingIdIs(m_3));
  EXPECT_THAT(vs.FindMapping(40, 50), MappingIdIs(m_2));
  EXPECT_THAT(vs.FindMapping(40, 70), MappingIdIs(m_4));
  EXPECT_THAT(vs.FindMapping(40, 80), MappingIdIs(m_4));
}

}  // namespace
}  // namespace perfetto::trace_processor::etm
