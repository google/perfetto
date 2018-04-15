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

#include "src/traced/probes/process_stats_data_source.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"

#include "src/process_stats/process_info.h"
#include "src/tracing/core/trace_writer_for_testing.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using ::testing::Invoke;

namespace perfetto {
namespace {

class TestProcessStatsDataSource : public ProcessStatsDataSource {
 public:
  TestProcessStatsDataSource(TracingSessionID id,
                             std::unique_ptr<TraceWriter> writer,
                             const DataSourceConfig& config)
      : ProcessStatsDataSource(id, std::move(writer), config) {}

  MOCK_METHOD1(ReadProcessInfo, std::unique_ptr<ProcessInfo>(int pid));
};

class ProcessStatsDataSourceTest : public ::testing::Test {
 protected:
  ProcessStatsDataSourceTest() {}

  TraceWriterForTesting* writer_raw_;

  std::unique_ptr<TestProcessStatsDataSource> GetProcessStatsDataSource(
      const DataSourceConfig& cfg) {
    auto writer =
        std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
    writer_raw_ = writer.get();
    return std::unique_ptr<TestProcessStatsDataSource>(
        new TestProcessStatsDataSource(0, std::move(writer), cfg));
  }

  static std::unique_ptr<ProcessInfo> GetProcessInfo(int pid) {
    ProcessInfo* process = new ProcessInfo();
    process->pid = pid;
    process->in_kernel = true;  // So that it doesn't try to read threads.
    process->ppid = 0;
    process->cmdline.push_back("test_process");
    return std::unique_ptr<ProcessInfo>(process);
  }
};

TEST_F(ProcessStatsDataSourceTest, TestWriteOnDemand) {
  DataSourceConfig config;
  auto data_source = GetProcessStatsDataSource(config);
  EXPECT_CALL(*data_source, ReadProcessInfo(42))
      .WillRepeatedly(Invoke(GetProcessInfo));
  data_source->OnPids({42});
  std::unique_ptr<protos::TracePacket> packet = writer_raw_->ParseProto();
  ASSERT_TRUE(packet->has_process_tree());
  ASSERT_EQ(packet->process_tree().processes_size(), 1);
  auto first_process = packet->process_tree().processes(0);
  ASSERT_EQ(first_process.pid(), 42);
  ASSERT_EQ(first_process.ppid(), 0);
  ASSERT_EQ(first_process.cmdline(0), std::string("test_process"));
}

}  // namespace
}  // namespace perfetto
