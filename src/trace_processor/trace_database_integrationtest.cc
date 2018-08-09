/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <map>
#include <string>

#include "perfetto/base/logging.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/utils.h"
#include "src/trace_processor/file_reader.h"
#include "src/trace_processor/trace_processor.h"

#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

class TraceProcessorIntegrationTest : public ::testing::Test {
 public:
  base::TestTaskRunner task_runner;
  TraceProcessor processor;

 protected:
  TraceProcessorIntegrationTest() : processor(&task_runner) {}

  void LoadTrace(const char* name) {
    FileReader reader(base::GetTestDataPath(name).c_str());
    auto loading_done = task_runner.CreateCheckpoint("loading_done");
    processor.LoadTrace(&reader, [loading_done]() { loading_done(); });
    task_runner.RunUntilCheckpoint("loading_done");
  }
};

TEST_F(TraceProcessorIntegrationTest, AndroidSchedAndPs) {
  LoadTrace("android_sched_and_ps.pb");
}

TEST_F(TraceProcessorIntegrationTest, Sfgate) {
  LoadTrace("sfgate.json");
  // TODO(hjd): Write some assertions here.
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
