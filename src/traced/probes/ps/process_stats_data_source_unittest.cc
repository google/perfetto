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

#include "src/traced/probes/ps/process_stats_data_source.h"

#include <dirent.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/temp_file.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/trace_writer_for_testing.h"

using ::testing::_;
using ::testing::ElementsAreArray;
using ::testing::Invoke;
using ::testing::Mock;
using ::testing::Return;
using ::testing::Truly;

namespace perfetto {
namespace {

class TestProcessStatsDataSource : public ProcessStatsDataSource {
 public:
  TestProcessStatsDataSource(base::TaskRunner* task_runner,
                             TracingSessionID id,
                             std::unique_ptr<TraceWriter> writer,
                             const DataSourceConfig& config)
      : ProcessStatsDataSource(task_runner, id, std::move(writer), config) {}

  MOCK_METHOD0(OpenProcDir, base::ScopedDir());
  MOCK_METHOD2(ReadProcPidFile, std::string(int32_t pid, const std::string&));
};

class ProcessStatsDataSourceTest : public ::testing::Test {
 protected:
  ProcessStatsDataSourceTest() {}

  std::unique_ptr<TestProcessStatsDataSource> GetProcessStatsDataSource(
      const DataSourceConfig& cfg) {
    auto writer =
        std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
    writer_raw_ = writer.get();
    return std::unique_ptr<TestProcessStatsDataSource>(
        new TestProcessStatsDataSource(&task_runner_, 0, std::move(writer),
                                       cfg));
  }

  base::TestTaskRunner task_runner_;
  TraceWriterForTesting* writer_raw_;
};

TEST_F(ProcessStatsDataSourceTest, WriteOnceProcess) {
  auto data_source = GetProcessStatsDataSource(DataSourceConfig());
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "status"))
      .WillOnce(Return("Name: foo\nTgid:\t42\nPid:   42\nPPid:  17\n"));
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "cmdline"))
      .WillOnce(Return(std::string("foo\0bar\0baz\0", 12)));

  data_source->OnPids({42});

  std::vector<protos::TracePacket> trace = writer_raw_->GetAllTracePackets();
  ASSERT_EQ(trace.size(), 1);
  auto ps_tree = trace[0].process_tree();
  ASSERT_EQ(ps_tree.processes_size(), 1);
  auto first_process = ps_tree.processes(0);
  ASSERT_EQ(first_process.pid(), 42);
  ASSERT_EQ(first_process.ppid(), 17);
  ASSERT_THAT(first_process.cmdline(), ElementsAreArray({"foo", "bar", "baz"}));
}

TEST_F(ProcessStatsDataSourceTest, DontRescanCachedPIDsAndTIDs) {
  // assertion helpers
  auto expected_process = [](int pid) {
    return [pid](protos::ProcessTree::Process process) {
      return process.pid() == pid && process.cmdline_size() > 0 &&
             process.cmdline(0) == "proc_" + std::to_string(pid);
    };
  };
  auto expected_thread = [](int tid) {
    return [tid](protos::ProcessTree::Thread thread) {
      return thread.tid() == tid && thread.tgid() == tid / 10 * 10 &&
             thread.name() == "thread_" + std::to_string(tid);
    };
  };

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

  // check written contents
  std::vector<protos::TracePacket> trace = writer_raw_->GetAllTracePackets();
  EXPECT_EQ(trace.size(), 3);

  // first packet - two unique processes, four threads
  auto ps_tree = trace[0].process_tree();
  EXPECT_THAT(ps_tree.processes(),
              UnorderedElementsAre(Truly(expected_process(10)),
                                   Truly(expected_process(20))));
  EXPECT_THAT(ps_tree.threads(),
              UnorderedElementsAre(
                  Truly(expected_thread(11)), Truly(expected_thread(12)),
                  Truly(expected_thread(21)), Truly(expected_thread(22))));

  // second packet - one new process
  ps_tree = trace[1].process_tree();
  EXPECT_THAT(ps_tree.processes(),
              UnorderedElementsAre(Truly(expected_process(30))));
  EXPECT_EQ(ps_tree.threads_size(), 0);

  // third packet - two threads that haven't been seen before
  ps_tree = trace[2].process_tree();
  EXPECT_EQ(ps_tree.processes_size(), 0);
  EXPECT_THAT(ps_tree.threads(),
              UnorderedElementsAre(Truly(expected_thread(31)),
                                   Truly(expected_thread(32))));
}

TEST_F(ProcessStatsDataSourceTest, IncrementalStateClear) {
  auto data_source = GetProcessStatsDataSource(DataSourceConfig());
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "status"))
      .WillOnce(Return("Name: foo\nTgid:\t42\nPid:   42\nPPid:  17\n"));
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "cmdline"))
      .WillOnce(Return(std::string("first_cmdline\0", 14)));

  data_source->OnPids({42});

  {
    std::vector<protos::TracePacket> trace = writer_raw_->GetAllTracePackets();
    ASSERT_EQ(trace.size(), 1);
    auto packet = trace[0];
    // First packet in the trace has no previous state, so the clear marker is
    // emitted.
    ASSERT_TRUE(packet.incremental_state_cleared());

    auto ps_tree = packet.process_tree();
    ASSERT_EQ(ps_tree.processes_size(), 1);
    ASSERT_EQ(ps_tree.processes(0).pid(), 42);
    ASSERT_EQ(ps_tree.processes(0).ppid(), 17);
    ASSERT_THAT(ps_tree.processes(0).cmdline(),
                ElementsAreArray({"first_cmdline"}));
  }

  // Look up the same pid, which shouldn't be re-emitted.
  Mock::VerifyAndClearExpectations(data_source.get());
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "status")).Times(0);
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "cmdline")).Times(0);

  data_source->OnPids({42});

  {
    std::vector<protos::TracePacket> trace = writer_raw_->GetAllTracePackets();
    ASSERT_EQ(trace.size(), 1);
  }

  // Invalidate incremental state, and look up the same pid again, which should
  // re-emit the proc tree info.
  Mock::VerifyAndClearExpectations(data_source.get());
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "status"))
      .WillOnce(Return("Name: foo\nTgid:\t42\nPid:   42\nPPid:  18\n"));
  EXPECT_CALL(*data_source, ReadProcPidFile(42, "cmdline"))
      .WillOnce(Return(std::string("second_cmdline\0", 15)));

  data_source->ClearIncrementalState();
  data_source->OnPids({42});

  {
    // Second packet with new proc information.
    std::vector<protos::TracePacket> trace = writer_raw_->GetAllTracePackets();
    ASSERT_EQ(trace.size(), 2);
    auto packet = trace[1];
    ASSERT_TRUE(packet.incremental_state_cleared());

    auto ps_tree = packet.process_tree();
    ASSERT_EQ(ps_tree.processes_size(), 1);
    ASSERT_EQ(ps_tree.processes(0).pid(), 42);
    ASSERT_EQ(ps_tree.processes(0).ppid(), 18);
    ASSERT_THAT(ps_tree.processes(0).cmdline(),
                ElementsAreArray({"second_cmdline"}));
  }
}

TEST_F(ProcessStatsDataSourceTest, RenamePids) {
  // assertion helpers
  auto expected_old_process = [](int pid) {
    return [pid](protos::ProcessTree::Process process) {
      return process.pid() == pid && process.cmdline_size() > 0 &&
             process.cmdline(0) == "proc_" + std::to_string(pid);
    };
  };
  auto expected_new_process = [](int pid) {
    return [pid](protos::ProcessTree::Process process) {
      return process.pid() == pid && process.cmdline_size() > 0 &&
             process.cmdline(0) == "new_" + std::to_string(pid);
    };
  };

  DataSourceConfig config;
  auto data_source = GetProcessStatsDataSource(config);
  for (int p : {10, 20}) {
    EXPECT_CALL(*data_source, ReadProcPidFile(p, "status"))
        .WillRepeatedly(Invoke([](int32_t pid, const std::string&) {
          return "Name: \tthread_" + std::to_string(pid) +
                 "\nTgid:  " + std::to_string(pid) +
                 "\nPid:   " + std::to_string(pid) + "\nPPid:  1\n";
        }));

    std::string old_proc_name = "proc_" + std::to_string(p);
    old_proc_name.resize(old_proc_name.size() + 1);  // Add a trailing \0.

    std::string new_proc_name = "new_" + std::to_string(p);
    new_proc_name.resize(new_proc_name.size() + 1);  // Add a trailing \0.
    EXPECT_CALL(*data_source, ReadProcPidFile(p, "cmdline"))
        .WillOnce(Return(old_proc_name))
        .WillOnce(Return(new_proc_name));
  }

  data_source->OnPids({10, 20});
  data_source->OnRenamePids({10});
  data_source->OnPids({10, 20});
  data_source->OnRenamePids({20});
  data_source->OnPids({10, 20});

  // check written contents
  std::vector<protos::TracePacket> trace = writer_raw_->GetAllTracePackets();
  EXPECT_EQ(trace.size(), 3);

  // first packet - two unique processes
  auto ps_tree = trace[0].process_tree();
  EXPECT_THAT(ps_tree.processes(),
              UnorderedElementsAre(Truly(expected_old_process(10)),
                                   Truly(expected_old_process(20))));
  EXPECT_EQ(ps_tree.threads_size(), 0);

  // second packet - one new process
  ps_tree = trace[1].process_tree();
  EXPECT_THAT(ps_tree.processes(),
              UnorderedElementsAre(Truly(expected_new_process(10))));
  EXPECT_EQ(ps_tree.threads_size(), 0);

  // third packet - two threads that haven't been seen before
  ps_tree = trace[2].process_tree();
  EXPECT_THAT(ps_tree.processes(),
              UnorderedElementsAre(Truly(expected_new_process(20))));
  EXPECT_EQ(ps_tree.threads_size(), 0);
}

TEST_F(ProcessStatsDataSourceTest, ProcessStats) {
  DataSourceConfig cfg;
  cfg.mutable_process_stats_config()->set_proc_stats_poll_ms(1);
  *(cfg.mutable_process_stats_config()->add_quirks()) =
      perfetto::ProcessStatsConfig::DISABLE_ON_DEMAND;
  auto data_source = GetProcessStatsDataSource(cfg);

  // Populate a fake /proc/ directory.
  auto fake_proc = base::TempDir::Create();
  const int kPids[] = {1, 2};
  std::vector<std::string> dirs_to_delete;
  for (int pid : kPids) {
    char path[256];
    sprintf(path, "%s/%d", fake_proc.path().c_str(), pid);
    dirs_to_delete.push_back(path);
    mkdir(path, 0755);
  }

  auto checkpoint = task_runner_.CreateCheckpoint("all_done");

  EXPECT_CALL(*data_source, OpenProcDir()).WillRepeatedly(Invoke([&fake_proc] {
    return base::ScopedDir(opendir(fake_proc.path().c_str()));
  }));

  const int kNumIters = 4;
  int iter = 0;
  for (int pid : kPids) {
    EXPECT_CALL(*data_source, ReadProcPidFile(pid, "status"))
        .WillRepeatedly(Invoke([checkpoint, &iter](int32_t p,
                                                   const std::string&) {
          char ret[1024];
          sprintf(ret, "Name:	pid_10\nVmSize:	 %d kB\nVmRSS:\t%d  kB\n",
                  p * 100 + iter * 10 + 1, p * 100 + iter * 10 + 2);
          return std::string(ret);
        }));

    EXPECT_CALL(*data_source, ReadProcPidFile(pid, "oom_score_adj"))
        .WillRepeatedly(Invoke(
            [checkpoint, kPids, &iter](int32_t inner_pid, const std::string&) {
              auto oom_score = inner_pid * 100 + iter * 10 + 3;
              if (inner_pid == kPids[base::ArraySize(kPids) - 1]) {
                if (++iter == kNumIters)
                  checkpoint();
              }
              return std::to_string(oom_score);
            }));
  }

  data_source->Start();
  task_runner_.RunUntilCheckpoint("all_done");
  data_source->Flush(1 /* FlushRequestId */, []() {});

  std::vector<protos::ProcessStats::Process> processes;
  std::vector<protos::TracePacket> trace = writer_raw_->GetAllTracePackets();
  for (const auto& packet : trace) {
    for (const auto& process : packet.process_stats().processes()) {
      processes.push_back(process);
    }
  }
  ASSERT_EQ(processes.size(), kNumIters * base::ArraySize(kPids));
  iter = 0;
  for (const auto& proc_counters : processes) {
    int32_t pid = proc_counters.pid();
    ASSERT_EQ(proc_counters.vm_size_kb(), pid * 100 + iter * 10 + 1);
    ASSERT_EQ(proc_counters.vm_rss_kb(), pid * 100 + iter * 10 + 2);
    ASSERT_EQ(proc_counters.oom_score_adj(), pid * 100 + iter * 10 + 3);
    if (pid == kPids[base::ArraySize(kPids) - 1])
      iter++;
  }

  // Cleanup |fake_proc|. TempDir checks that the directory is empty.
  for (std::string& path : dirs_to_delete)
    rmdir(path.c_str());
}

TEST_F(ProcessStatsDataSourceTest, CacheProcessStats) {
  DataSourceConfig cfg;
  cfg.mutable_process_stats_config()->set_proc_stats_poll_ms(105);
  cfg.mutable_process_stats_config()->set_proc_stats_cache_ttl_ms(220);
  *(cfg.mutable_process_stats_config()->add_quirks()) =
      perfetto::ProcessStatsConfig::DISABLE_ON_DEMAND;
  auto data_source = GetProcessStatsDataSource(cfg);

  // Populate a fake /proc/ directory.
  auto fake_proc = base::TempDir::Create();
  const int kPid = 1;

  char path[256];
  sprintf(path, "%s/%d", fake_proc.path().c_str(), kPid);
  mkdir(path, 0755);

  auto checkpoint = task_runner_.CreateCheckpoint("all_done");

  EXPECT_CALL(*data_source, OpenProcDir()).WillRepeatedly(Invoke([&fake_proc] {
    return base::ScopedDir(opendir(fake_proc.path().c_str()));
  }));

  const int kNumIters = 4;
  int iter = 0;
  EXPECT_CALL(*data_source, ReadProcPidFile(kPid, "status"))
      .WillRepeatedly(Invoke([checkpoint](int32_t p, const std::string&) {
        char ret[1024];
        sprintf(ret, "Name:	pid_10\nVmSize:	 %d kB\nVmRSS:\t%d  kB\n",
                p * 100 + 1, p * 100 + 2);
        return std::string(ret);
      }));

  EXPECT_CALL(*data_source, ReadProcPidFile(kPid, "oom_score_adj"))
      .WillRepeatedly(
          Invoke([checkpoint, &iter](int32_t inner_pid, const std::string&) {
            if (++iter == kNumIters)
              checkpoint();
            return std::to_string(inner_pid * 100);
          }));

  data_source->Start();
  task_runner_.RunUntilCheckpoint("all_done");
  data_source->Flush(1 /* FlushRequestId */, []() {});

  std::vector<protos::ProcessStats::Process> processes;
  std::vector<protos::TracePacket> trace = writer_raw_->GetAllTracePackets();
  for (const auto& packet : trace) {
    for (const auto& process : packet.process_stats().processes()) {
      processes.push_back(process);
    }
  }
  // We should get two counter events because:
  // a) emissions happen at 0ms, 105ms, 210ms, 315ms
  // b) clear events happen at 220ms, 440ms...
  // Therefore, we should see the emissions at 0ms and 315ms.
  ASSERT_EQ(processes.size(), 2);
  for (const auto& proc_counters : processes) {
    ASSERT_EQ(proc_counters.pid(), kPid);
    ASSERT_EQ(proc_counters.vm_size_kb(), kPid * 100 + 1);
    ASSERT_EQ(proc_counters.vm_rss_kb(), kPid * 100 + 2);
    ASSERT_EQ(proc_counters.oom_score_adj(), kPid * 100);
  }

  // Cleanup |fake_proc|. TempDir checks that the directory is empty.
  rmdir(path);
}

}  // namespace
}  // namespace perfetto
