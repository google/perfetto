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
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "src/tracing/core/trace_writer_for_testing.h"

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::ElementsAreArray;

namespace perfetto {
namespace {

class TestProcessStatsDataSource : public ProcessStatsDataSource {
 public:
  TestProcessStatsDataSource(TracingSessionID id,
                             std::unique_ptr<TraceWriter> writer,
                             const DataSourceConfig& config)
      : ProcessStatsDataSource(id, std::move(writer), config) {}

  MOCK_METHOD2(ReadProcPidFile, std::string(int32_t pid, const std::string&));
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
};

TEST_F(ProcessStatsDataSourceTest, WriteOnceProcess) {
  auto data_source = GetProcessStatsDataSource(DataSourceConfig());
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "status"))
      .WillOnce(Return("Name: foo\nTgid:\t42\nPid:   42\nPPid:  17\n"));
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "cmdline"))
      .WillOnce(Return(std::string("foo\0bar\0baz\0", 12)));

  data_source->OnPids({42});
  std::unique_ptr<protos::TracePacket> packet = writer_raw_->ParseProto();
  ASSERT_TRUE(packet->has_process_tree());
  ASSERT_EQ(packet->process_tree().processes_size(), 1);
  auto first_process = packet->process_tree().processes(0);
  ASSERT_EQ(first_process.pid(), 42);
  ASSERT_EQ(first_process.ppid(), 17);
  EXPECT_THAT(first_process.cmdline(), ElementsAreArray({"foo", "bar", "baz"}));
}

TEST_F(ProcessStatsDataSourceTest, DontRescanCachedPIDsAndTIDs) {
  DataSourceConfig config;
  config.mutable_process_stats_config()->set_record_thread_names(true);
  auto data_source = GetProcessStatsDataSource(config);
  for (int p : {10, 11, 12, 20, 21, 22, 30, 31, 32}) {
    EXPECT_CALL(*data_source, ReadProcPidFile(p, "status"))
        .WillOnce(Invoke([](int32_t pid, const std::string&) {
          int32_t tgid = (pid / 10) * 10;
          return "Name: \tthread_" + std::to_string(pid) +
                 "\nTgid:  " + std::to_string(tgid) +
                 "\nPid:   " + std::to_string(pid) + "\nPPid:  1\n";
        }));
    if (p % 10 == 0) {
      std::string proc_name = "proc_" + std::to_string(p);
      proc_name.resize(proc_name.size() + 1);  // Add a trailing \0.
      EXPECT_CALL(*data_source, ReadProcPidFile(p, "cmdline"))
          .WillOnce(Return(proc_name));
    }
  }

  data_source->OnPids({10, 11, 12, 20, 21, 22, 10, 20, 11, 21});
  data_source->OnPids({30});
  data_source->OnPids({10, 30, 10, 31, 32});

  std::unique_ptr<protos::TracePacket> packet = writer_raw_->ParseProto();
  ASSERT_TRUE(packet->has_process_tree());
  const auto& proceses = packet->process_tree().processes();
  const auto& threads = packet->process_tree().threads();
  ASSERT_EQ(proceses.size(), 3);
  int tid_idx = 0;
  for (int pid_idx = 0; pid_idx < 3; pid_idx++) {
    int pid = (pid_idx + 1) * 10;
    std::string proc_name = "proc_" + std::to_string(pid);
    ASSERT_EQ(proceses.Get(pid_idx).pid(), pid);
    ASSERT_EQ(proceses.Get(pid_idx).cmdline().Get(0), proc_name);
    for (int tid = pid + 1; tid < pid + 3; tid++, tid_idx++) {
      ASSERT_EQ(threads.Get(tid_idx).tid(), tid);
      ASSERT_EQ(threads.Get(tid_idx).tgid(), pid);
      ASSERT_EQ(threads.Get(tid_idx).name(), "thread_" + std::to_string(tid));
    }
  }
}

}  // namespace
}  // namespace perfetto
