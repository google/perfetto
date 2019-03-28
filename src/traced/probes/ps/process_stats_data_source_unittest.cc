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

#include "perfetto/base/temp_file.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/trace_writer_for_testing.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"

using ::testing::_;
using ::testing::ElementsAreArray;
using ::testing::Invoke;
using ::testing::Return;

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

  // |packet| will contain the merge of all kNumIter packets written.
  std::unique_ptr<protos::TracePacket> packet = writer_raw_->ParseProto();
  ASSERT_TRUE(packet);
  ASSERT_TRUE(packet->has_process_stats());
  const auto& ps_stats = packet->process_stats();
  ASSERT_EQ(ps_stats.processes_size(), kNumIters * base::ArraySize(kPids));
  iter = 0;
  for (const auto& proc_counters : ps_stats.processes()) {
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

  std::unique_ptr<protos::TracePacket> packet = writer_raw_->ParseProto();
  ASSERT_TRUE(packet);
  ASSERT_TRUE(packet->has_process_stats());
  const auto& ps_stats = packet->process_stats();
  ASSERT_EQ(ps_stats.processes_size(), 2);

  // We should get two counter events because:
  // a) emissions happen at 0ms, 105ms, 210ms, 315ms
  // b) clear events happen at 220ms, 440ms...
  // Therefore, we should see the emissions at 0ms and 315ms.
  for (const auto& proc_counters : ps_stats.processes()) {
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
