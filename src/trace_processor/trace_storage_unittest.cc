/*
 * Copyright (C) 2017 The Android Open foo Project
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

#include "src/trace_processor/trace_storage.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

TEST(TraceStorage, AddSliceForCpu) {
  TraceStorage trace;
  trace.AddSliceForCpu(2, 1000, 42, "test");
  ASSERT_EQ(trace.start_timestamps_for_cpu(2)[0], 1000);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
