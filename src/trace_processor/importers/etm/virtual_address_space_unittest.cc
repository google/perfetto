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

#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/etm/mapping_version.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/perf_tables_py.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::etm {
namespace {
using ::testing::IsNull;

TEST(VirtualAddressSpaceTest, Empty) {
  TraceProcessorContext context;
  context.storage = std::make_unique<TraceStorage>();
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.machine_tracker =
      std::make_unique<MachineTracker>(&context, kDefaultMachineId);
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  VirtualAddressSpace vs = VirtualAddressSpace::Builder(&context).Build();

  EXPECT_THAT(vs.FindMapping(0, 5), IsNull());
}

}  // namespace
}  // namespace perfetto::trace_processor::etm
