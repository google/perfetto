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

#include <unistd.h>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/base/test/test_task_runner.h"
#include "src/traced/probes/common/cpu_freq_info_for_testing.h"
#include "src/traced/probes/sys_stats/sys_stats_data_source.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/sys_stats_counters.gen.h"
#include "protos/perfetto/config/data_source_config.gen.h"
#include "protos/perfetto/config/sys_stats/sys_stats_config.gen.h"
#include "protos/perfetto/trace/sys_stats/sys_stats.gen.h"

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::UnorderedElementsAre;

namespace perfetto {
namespace {

const char kMockMeminfo[] = R"(
MemTotal:        3744240 kB
MemFree:           73328 kB
MemAvailable:     629896 kB
Buffers:           19296 kB
Cached:           731032 kB
SwapCached:         4936 kB
Active:          1616348 kB
Inactive:         745492 kB
Active(anon):    1322636 kB
Inactive(anon):   449172 kB
Active(file):     293712 kB
Inactive(file):   296320 kB
Unevictable:      142152 kB
Mlocked:          142152 kB
SwapTotal:        524284 kB
SwapFree:            128 kB
Dirty:                 0 kB
Writeback:             0 kB
AnonPages:       1751140 kB
Mapped:           508372 kB
Shmem:             18604 kB
Slab:             240352 kB
SReclaimable:      64684 kB
SUnreclaim:       175668 kB
KernelStack:       62672 kB
PageTables:        70108 kB
NFS_Unstable:          0 kB
Bounce:                0 kB
WritebackTmp:          0 kB
CommitLimit:     2396404 kB
Committed_AS:   81911488 kB
VmallocTotal:   258867136 kB
VmallocUsed:           0 kB
VmallocChunk:          0 kB
CmaTotal:         196608 kB
CmaFree:              60 kB)";

const char kMockVmstat[] = R"(
nr_free_pages 16449
nr_alloc_batch 79
nr_inactive_anon 112545
nr_active_anon 322027
nr_inactive_file 75904
nr_active_file 87939
nr_unevictable 35538
nr_mlock 35538
nr_anon_pages 429005
nr_mapped 125844
nr_file_pages 205523
nr_dirty 23
nr_writeback 0
nr_slab_reclaimable 15840
nr_slab_unreclaimable 43912
nr_page_table_pages 17158
nr_kernel_stack 3822
nr_overhead 0
nr_unstable 0
nr_bounce 0
nr_vmscan_write 558690
nr_vmscan_immediate_reclaim 14853
nr_writeback_temp 0
nr_isolated_anon 0
nr_isolated_file 0
nr_shmem 5027
nr_dirtied 6732417
nr_written 6945513
nr_pages_scanned 0
workingset_refault 32784684
workingset_activate 8200928
workingset_nodereclaim 0
nr_anon_transparent_hugepages 0
nr_free_cma 0
nr_swapcache 1254
nr_dirty_threshold 33922
nr_dirty_background_threshold 8449
pgpgin 161257156
pgpgout 35973852
pgpgoutclean 37181384
pswpin 185308
pswpout 557662
pgalloc_dma 79259070
pgalloc_normal 88265512
pgalloc_movable 0
pgfree 175051592
pgactivate 11897892
pgdeactivate 20412230
pgfault 181696234
pgmajfault 1060871
pgrefill_dma 12970047
pgrefill_normal 14391564
pgrefill_movable 0
pgsteal_kswapd_dma 19471476
pgsteal_kswapd_normal 21138380
pgsteal_kswapd_movable 0
pgsteal_direct 91537
pgsteal_direct_dma 40625
pgsteal_direct_normal 50912
pgsteal_direct_movable 0
pgscan_kswapd_dma 23544417
pgscan_kswapd_normal 25623715
pgscan_kswapd_movable 0
pgscan_direct_dma 50369
pgscan_direct_normal 66284
pgscan_direct_movable 0
pgscan_direct_throttle 0
pginodesteal 0
slabs_scanned 39582828
kswapd_inodesteal 110199
kswapd_low_wmark_hit_quickly 21321
kswapd_high_wmark_hit_quickly 4112
pageoutrun 37666
allocstall 1587
pgrotated 12086
drop_pagecache 0
drop_slab 0
pgmigrate_success 5923482
pgmigrate_fail 3439
compact_migrate_scanned 92906456
compact_free_scanned 467077168
compact_isolated 13456528
compact_stall 197
compact_fail 42
compact_success 155
compact_daemon_wake 2131
unevictable_pgs_culled 50170
unevictable_pgs_scanned 0
unevictable_pgs_rescued 14640
unevictable_pgs_mlocked 52520
unevictable_pgs_munlocked 14640
unevictable_pgs_cleared 2342
unevictable_pgs_stranded 2342
vma_lock_abort 1173728)";

const char kMockStat[] = R"(
cpu  2655987 822682 2352153 8801203 41917 322733 175055 0 0 0
cpu0 762178 198125 902284 8678856 41716 152974 68262 72386 0 0
cpu1 613833 243394 504323 15194 96 60625 28785 0 0 0
cpu2 207349 95060 248856 17351 42 32148 26108 0 0 0
cpu3 138474 92158 174852 17537 48 25076 25035 0 0 0
cpu4 278720 34689 141048 18117 1 20782 5873 0 0 0
cpu5 235376 33907 85098 18278 2 10049 3774 0 0 0
cpu6 239568 67149 155814 17890 5 11518 3807 0 0 0
cpu7 180484 58196 139874 17975 3 9556 13407 28643 0 0
intr 238128517 0 0 0 63500984 0 6253792 6 4 5 0 0 0 0 0 0 0 160331 0 0 14 0 0 0 0 0 0 0 0 0 0 0 20430 2279 11 11 83272 0 0 0 0 0 0 0 5754 220829 0 154753 908545 1824602 7314228 0 0 0 6898259 0 0 10 0 0 2 0 0 0 0 0 0 0 42 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6 575816 1447531 134022 0 0 0 0 0 435008 319921 2755476 0 0 0 0 91 310212 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6 4 0 0 545 901 554 9 3377 4184 12 10 588851 0 2 1109045 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 6 8 0 0 0 0 0 0 0 0 0 0 0 0 497 0 0 0 0 0 26172 0 0 0 0 0 0 0 1362 0 0 0 0 0 0 0 424 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 23427 0 0 0 0 1 1298 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 108 0 0 0 0 86 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1784935 407979 2140 10562241 52374 74699 6976 84926 222 169088 0 0 0 0 174 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 2789 51543 0 83 0 0 0 0 0 0 0 0 0 0 0 0 0 0 8 8 0 13 11 17 1393 0 0 0 0 0 0 0 0 0 0 26 0 0 2 106 0 0 0 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 11150 0 13 0 1 390 6 0 6 4 0 0 0 0 352 284743 2 0 0 24 3 0 3 0 0 0 12 0 668788 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 680 0 0
ctxt 373122860
btime 1536912218
processes 243320
procs_running 1
procs_blocked 0
softirq 84611084 10220177 28299167 155083 3035679 6390543 66234 4396819 15604187 0 16443195)";

const char kMockBuddy[] = R"(
Node 0, zone  DMA      2743  1659  2063  685   27   4  0  0  0  0  0
Node 0, zone  Normal   143   744   89    1080  105  1  0  2  0  2  2
Node 0, zone  HighMem  345   90    156   3     5    2  0  0  0  0  0
Node 1, zone  Normal   233   123   453   10    5    1  0  2  0  0  3)";

const char kDevfreq1[] = "1000000";
const char kDevfreq2[] = "20000000";

const char kMockDiskStat[] = R"(
 253       0 zram0 13886 0 111088 128 57298 0 458384 48 0 15248 176 0 0 0 0 0 0
   8       0 sda 54133 5368 8221736 75929 30333 1157434 9599744 143190 0 63672 249858 9595 0 2160072 19411 6649 11327
   8       1 sda1 18 6 632 7 39 49 704 92 0 156 100 0 0 0 0 0 0)";

const char kMockPsi[] = R"(
some avg10=23.10 avg60=5.06 avg300=15.10 total=417963
full avg10=9.00 avg60=19.20 avg300=3.23 total=205933)";

const uint64_t kMockThermalTemp = 25000;
const char kMockThermalType[] = "TSR0";
const uint64_t kMockCpuIdleStateTime = 10000;
const char kMockCpuIdleStateName[] = "MOCK_STATE_NAME";
const uint64_t kMockIntelGpuFreq = 300;
// kMockAMDGpuFreq whitespace is intentional.
const char kMockAMDGpuFreq[] = R"(
0: 200Mhz 
1: 400Mhz *
2: 2000Mhz 
)";
class TestSysStatsDataSource : public SysStatsDataSource {
 public:
  TestSysStatsDataSource(base::TaskRunner* task_runner,
                         TracingSessionID id,
                         std::unique_ptr<TraceWriter> writer,
                         const DataSourceConfig& config,
                         std::unique_ptr<CpuFreqInfo> cpu_freq_info,
                         OpenFunction open_fn)
      : SysStatsDataSource(task_runner,
                           id,
                           std::move(writer),
                           config,
                           std::move(cpu_freq_info),
                           open_fn) {}

  MOCK_METHOD(base::ScopedDir,
              OpenDirAndLogOnErrorOnce,
              (const std::string& dir_path, bool* already_logged),
              (override));
  MOCK_METHOD(const char*,
              ReadDevfreqCurFreq,
              (const std::string& deviceName),
              (override));
  MOCK_METHOD(std::optional<uint64_t>,
              ReadFileToUInt64,
              (const std::string& name),
              (override));
  MOCK_METHOD(std::optional<std::string>,
              ReadFileToString,
              (const std::string& name),
              (override));
  bool* GetDevfreqErrorLoggedAddress() { return &devfreq_error_logged_; }
  bool* GetThermalErrorLoggedAddress() { return &thermal_error_logged_; }
  bool* GetCpuIdleErrorLoggedAddress() { return &cpuidle_error_logged_; }
};

base::ScopedFile MockOpenReadOnly(const char* path) {
  base::TempFile tmp_ = base::TempFile::CreateUnlinked();
  if (!strcmp(path, "/proc/meminfo")) {
    EXPECT_GT(pwrite(tmp_.fd(), kMockMeminfo, strlen(kMockMeminfo), 0), 0);
  } else if (!strcmp(path, "/proc/vmstat")) {
    EXPECT_GT(pwrite(tmp_.fd(), kMockVmstat, strlen(kMockVmstat), 0), 0);
  } else if (!strcmp(path, "/proc/stat")) {
    EXPECT_GT(pwrite(tmp_.fd(), kMockStat, strlen(kMockStat), 0), 0);
  } else if (!strcmp(path, "/proc/buddyinfo")) {
    EXPECT_GT(pwrite(tmp_.fd(), kMockBuddy, strlen(kMockBuddy), 0), 0);
  } else if (!strcmp(path, "/proc/diskstats")) {
    EXPECT_GT(pwrite(tmp_.fd(), kMockDiskStat, strlen(kMockDiskStat), 0), 0);
  } else if (base::StartsWith(path, "/proc/pressure/")) {
    EXPECT_GT(pwrite(tmp_.fd(), kMockPsi, strlen(kMockPsi), 0), 0);
  } else {
    PERFETTO_FATAL("Unexpected file opened %s", path);
  }
  return tmp_.ReleaseFD();
}

class SysStatsDataSourceTest : public ::testing::Test {
 protected:
  std::unique_ptr<TestSysStatsDataSource> GetSysStatsDataSource(
      const DataSourceConfig& cfg) {
    auto writer =
        std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
    writer_raw_ = writer.get();
    auto instance =
        std::unique_ptr<TestSysStatsDataSource>(new TestSysStatsDataSource(
            &task_runner_, 0, std::move(writer), cfg,
            cpu_freq_info_for_testing_.GetInstance(), MockOpenReadOnly));
    instance->set_ns_per_user_hz_for_testing(1000000000ull / 100);  // 100 Hz.
    instance->Start();
    return instance;
  }

  void Poller(SysStatsDataSource* ds, std::function<void()> checkpoint) {
    if (ds->tick_for_testing())
      checkpoint();
    else
      task_runner_.PostDelayedTask(
          [ds, checkpoint, this] { Poller(ds, checkpoint); }, 1);
  }

  void WaitTick(SysStatsDataSource* data_source) {
    auto checkpoint = task_runner_.CreateCheckpoint("on_tick");
    Poller(data_source, checkpoint);
    task_runner_.RunUntilCheckpoint("on_tick");
  }

  TraceWriterForTesting* writer_raw_ = nullptr;
  base::TestTaskRunner task_runner_;
  CpuFreqInfoForTesting cpu_freq_info_for_testing_;
};

TEST_F(SysStatsDataSourceTest, Meminfo) {
  using C = protos::gen::MeminfoCounters;
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_meminfo_period_ms(10);
  sys_cfg.add_meminfo_counters(C::MEMINFO_MEM_TOTAL);
  sys_cfg.add_meminfo_counters(C::MEMINFO_MEM_FREE);
  sys_cfg.add_meminfo_counters(C::MEMINFO_ACTIVE_ANON);
  sys_cfg.add_meminfo_counters(C::MEMINFO_INACTIVE_FILE);
  sys_cfg.add_meminfo_counters(C::MEMINFO_CMA_FREE);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.vmstat_size(), 0);
  EXPECT_EQ(sys_stats.buddy_info_size(), 0);
  EXPECT_EQ(sys_stats.cpu_stat_size(), 0);
  EXPECT_EQ(sys_stats.devfreq_size(), 0);

  using KV = std::pair<int, uint64_t>;
  std::vector<KV> kvs;
  for (const auto& kv : sys_stats.meminfo())
    kvs.push_back({kv.key(), kv.value()});

  EXPECT_THAT(kvs,
              UnorderedElementsAre(KV{C::MEMINFO_MEM_TOTAL, 3744240},     //
                                   KV{C::MEMINFO_MEM_FREE, 73328},        //
                                   KV{C::MEMINFO_ACTIVE_ANON, 1322636},   //
                                   KV{C::MEMINFO_INACTIVE_FILE, 296320},  //
                                   KV{C::MEMINFO_CMA_FREE, 60}));
}

TEST_F(SysStatsDataSourceTest, MeminfoAll) {
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_meminfo_period_ms(10);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.vmstat_size(), 0);
  EXPECT_EQ(sys_stats.buddy_info_size(), 0);
  EXPECT_EQ(sys_stats.cpu_stat_size(), 0);
  EXPECT_EQ(sys_stats.devfreq_size(), 0);
  EXPECT_GE(sys_stats.meminfo_size(), 10);
}

TEST_F(SysStatsDataSourceTest, Vmstat) {
  using C = protos::gen::VmstatCounters;
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_vmstat_period_ms(10);
  sys_cfg.add_vmstat_counters(C::VMSTAT_NR_FREE_PAGES);
  sys_cfg.add_vmstat_counters(C::VMSTAT_PGACTIVATE);
  sys_cfg.add_vmstat_counters(C::VMSTAT_PGMIGRATE_FAIL);
  sys_cfg.add_vmstat_counters(C::VMSTAT_PGSTEAL_DIRECT);
  sys_cfg.add_vmstat_counters(C::VMSTAT_VMA_LOCK_ABORT);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.meminfo_size(), 0);
  EXPECT_EQ(sys_stats.cpu_stat_size(), 0);
  EXPECT_EQ(sys_stats.devfreq_size(), 0);

  using KV = std::pair<int, uint64_t>;
  std::vector<KV> kvs;
  for (const auto& kv : sys_stats.vmstat())
    kvs.push_back({kv.key(), kv.value()});

  EXPECT_THAT(kvs,
              UnorderedElementsAre(KV{C::VMSTAT_NR_FREE_PAGES, 16449},    //
                                   KV{C::VMSTAT_PGACTIVATE, 11897892},    //
                                   KV{C::VMSTAT_PGMIGRATE_FAIL, 3439},    //
                                   KV{C::VMSTAT_PGSTEAL_DIRECT, 91537},   //
                                   KV{C::VMSTAT_VMA_LOCK_ABORT, 1173728}  //
                                   ));
}

TEST_F(SysStatsDataSourceTest, VmstatAll) {
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_vmstat_period_ms(10);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.meminfo_size(), 0);
  EXPECT_EQ(sys_stats.cpu_stat_size(), 0);
  EXPECT_EQ(sys_stats.devfreq_size(), 0);
  EXPECT_EQ(sys_stats.buddy_info_size(), 0);
  EXPECT_GE(sys_stats.vmstat_size(), 10);
}

TEST_F(SysStatsDataSourceTest, BuddyinfoAll) {
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_buddyinfo_period_ms(10);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.meminfo_size(), 0);
  EXPECT_EQ(sys_stats.cpu_stat_size(), 0);
  EXPECT_EQ(sys_stats.devfreq_size(), 0);
  EXPECT_GE(sys_stats.vmstat_size(), 0);
  EXPECT_EQ(sys_stats.buddy_info_size(), 4);

  EXPECT_EQ(sys_stats.buddy_info()[0].node(), "0");
  EXPECT_EQ(sys_stats.buddy_info()[0].zone(), "DMA");
  EXPECT_EQ(sys_stats.buddy_info()[0].order_pages()[0], 2743u);
  EXPECT_EQ(sys_stats.buddy_info()[0].order_pages()[5], 4u);
  EXPECT_EQ(sys_stats.buddy_info()[0].order_pages()[10], 0u);

  EXPECT_EQ(sys_stats.buddy_info()[1].node(), "0");
  EXPECT_EQ(sys_stats.buddy_info()[1].zone(), "Normal");
  EXPECT_EQ(sys_stats.buddy_info()[1].order_pages()[0], 143u);
  EXPECT_EQ(sys_stats.buddy_info()[1].order_pages()[5], 1u);
  EXPECT_EQ(sys_stats.buddy_info()[1].order_pages()[10], 2u);

  EXPECT_EQ(sys_stats.buddy_info()[2].node(), "0");
  EXPECT_EQ(sys_stats.buddy_info()[2].zone(), "HighMem");
  EXPECT_EQ(sys_stats.buddy_info()[2].order_pages()[0], 345u);
  EXPECT_EQ(sys_stats.buddy_info()[2].order_pages()[5], 2u);
  EXPECT_EQ(sys_stats.buddy_info()[2].order_pages()[10], 0u);

  EXPECT_EQ(sys_stats.buddy_info()[3].node(), "1");
  EXPECT_EQ(sys_stats.buddy_info()[3].zone(), "Normal");
  EXPECT_EQ(sys_stats.buddy_info()[3].order_pages()[0], 233u);
  EXPECT_EQ(sys_stats.buddy_info()[3].order_pages()[5], 1u);
  EXPECT_EQ(sys_stats.buddy_info()[3].order_pages()[10], 3u);
}

TEST_F(SysStatsDataSourceTest, ThermalZones) {
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_thermal_period_ms(10);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  // Create dirs and symlinks, but only read the symlinks.
  std::vector<std::string> dirs_to_delete;
  std::vector<std::string> symlinks_to_delete;
  auto make_thermal_paths = [&symlinks_to_delete, &dirs_to_delete](
                                base::TempDir& temp_dir, base::TempDir& sym_dir,
                                const char* name) {
    base::StackString<256> path("%s/%s", temp_dir.path().c_str(), name);
    dirs_to_delete.push_back(path.ToStdString());
    mkdir(path.c_str(), 0755);
    base::StackString<256> sym_path("%s/%s", sym_dir.path().c_str(), name);
    symlinks_to_delete.push_back(sym_path.ToStdString());
    symlink(path.c_str(), sym_path.c_str());
  };
  auto fake_thermal = base::TempDir::Create();
  auto fake_thermal_symdir = base::TempDir::Create();
  static const char* const thermalzone_names[] = {"thermal_zone0"};
  for (auto dev : thermalzone_names) {
    make_thermal_paths(fake_thermal, fake_thermal_symdir, dev);
  }

  EXPECT_CALL(*data_source, OpenDirAndLogOnErrorOnce(
                                "/sys/class/thermal/",
                                data_source->GetThermalErrorLoggedAddress()))
      .WillRepeatedly(Invoke([&fake_thermal_symdir] {
        return base::ScopedDir(opendir(fake_thermal_symdir.path().c_str()));
      }));

  EXPECT_CALL(*data_source,
              ReadFileToUInt64("/sys/class/thermal/thermal_zone0/temp"))
      .WillRepeatedly(Return(std::optional<uint64_t>(kMockThermalTemp)));
  EXPECT_CALL(*data_source,
              ReadFileToString("/sys/class/thermal/thermal_zone0/type"))
      .WillRepeatedly(Return(std::optional<std::string>(kMockThermalType)));

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();

  ASSERT_EQ(sys_stats.thermal_zone_size(), 1);
  EXPECT_EQ(sys_stats.thermal_zone()[0].name(), "thermal_zone0");
  EXPECT_EQ(sys_stats.thermal_zone()[0].temp(), kMockThermalTemp / 1000);
  EXPECT_EQ(sys_stats.thermal_zone()[0].type(), kMockThermalType);

  for (const std::string& path : dirs_to_delete)
    base::Rmdir(path);
  for (const std::string& path : symlinks_to_delete)
    remove(path.c_str());
}

TEST_F(SysStatsDataSourceTest, CpuIdleStates) {
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_cpuidle_period_ms(10);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  // Create dirs.
  std::vector<std::string> dirs_to_delete;
  auto make_cpuidle_paths = [&dirs_to_delete](base::TempDir& temp_dir,
                                              std::string name) {
    std::string path = temp_dir.path() + "/" + name;
    dirs_to_delete.push_back(path);
    mkdir(path.c_str(), 0755);
  };
  auto fake_cpuidle = base::TempDir::Create();

  std::string cpu_name[3] = {"/cpu0", "/cpu0/cpuidle", "/cpu0/cpuidle/state0"};
  for (const std::string& path : cpu_name) {
    make_cpuidle_paths(fake_cpuidle, path);
  }

  EXPECT_CALL(*data_source, OpenDirAndLogOnErrorOnce(
                                "/sys/devices/system/cpu/",
                                data_source->GetCpuIdleErrorLoggedAddress()))
      .WillOnce(Invoke([&fake_cpuidle] {
        return base::ScopedDir(opendir(fake_cpuidle.path().c_str()));
      }));

  EXPECT_CALL(*data_source, OpenDirAndLogOnErrorOnce(
                                "/sys/devices/system/cpu/cpu0/cpuidle/",
                                data_source->GetCpuIdleErrorLoggedAddress()))
      .WillRepeatedly(Invoke([&fake_cpuidle] {
        std::string path = fake_cpuidle.path() + "/cpu0/cpuidle";
        return base::ScopedDir(opendir(path.c_str()));
      }));

  EXPECT_CALL(
      *data_source,
      ReadFileToUInt64("/sys/devices/system/cpu/cpu0/cpuidle/state0/time"))
      .WillRepeatedly(Return(std::optional<uint64_t>(kMockCpuIdleStateTime)));
  EXPECT_CALL(
      *data_source,
      ReadFileToString("/sys/devices/system/cpu/cpu0/cpuidle/state0/name"))
      .WillRepeatedly(
          Return(std::optional<std::string>(kMockCpuIdleStateName)));

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.cpuidle_state_size(), 1);
  uint32_t cpu_id = 0;
  EXPECT_EQ(sys_stats.cpuidle_state()[0].cpu_id(), cpu_id);
  EXPECT_EQ(sys_stats.cpuidle_state()[0].cpuidle_state_entry_size(), 1);
  EXPECT_EQ(sys_stats.cpuidle_state()[0].cpuidle_state_entry()[0].state(),
            kMockCpuIdleStateName);
  EXPECT_EQ(sys_stats.cpuidle_state()[0].cpuidle_state_entry()[0].duration_us(),
            kMockCpuIdleStateTime);

  for (auto i = dirs_to_delete.size(); i > 0; i--) {
    base::Rmdir(dirs_to_delete[i - 1]);
  }
}

TEST_F(SysStatsDataSourceTest, IntelGpuFrequency) {
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_gpufreq_period_ms(10);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  EXPECT_CALL(*data_source,
              ReadFileToUInt64("/sys/class/drm/card0/gt_act_freq_mhz"))
      .WillRepeatedly(Return(std::optional<uint64_t>(kMockIntelGpuFreq)));

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.gpufreq_mhz_size(), 1);
  uint32_t intel_gpufreq = 300;
  EXPECT_EQ(sys_stats.gpufreq_mhz()[0], intel_gpufreq);
}

TEST_F(SysStatsDataSourceTest, AMDGpuFrequency) {
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_gpufreq_period_ms(10);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  // Ignore other GPU freq calls.
  EXPECT_CALL(*data_source,
              ReadFileToUInt64("/sys/class/drm/card0/gt_act_freq_mhz"));
  EXPECT_CALL(*data_source,
              ReadFileToString("/sys/class/drm/card0/device/pp_dpm_sclk"))
      .WillRepeatedly(Return(std::optional<std::string>(kMockAMDGpuFreq)));

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.gpufreq_mhz_size(), 1);
  uint32_t amd_gpufreq = 400;
  EXPECT_EQ(sys_stats.gpufreq_mhz()[0], amd_gpufreq);
}

TEST_F(SysStatsDataSourceTest, DevfreqAll) {
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_devfreq_period_ms(10);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  // Create dirs and symlinks, but only read the symlinks.
  std::vector<std::string> dirs_to_delete;
  std::vector<std::string> symlinks_to_delete;
  auto make_devfreq_paths = [&symlinks_to_delete, &dirs_to_delete](
                                base::TempDir& temp_dir, base::TempDir& sym_dir,
                                const char* name) {
    base::StackString<256> path("%s/%s", temp_dir.path().c_str(), name);
    dirs_to_delete.push_back(path.ToStdString());
    mkdir(path.c_str(), 0755);
    base::StackString<256> sym_path("%s/%s", sym_dir.path().c_str(), name);
    symlinks_to_delete.push_back(sym_path.ToStdString());
    symlink(path.c_str(), sym_path.c_str());
  };
  auto fake_devfreq = base::TempDir::Create();
  auto fake_devfreq_symdir = base::TempDir::Create();
  static const char* const devfreq_names[] = {"10010.devfreq_device_a",
                                              "10020.devfreq_device_b"};
  for (auto dev : devfreq_names) {
    make_devfreq_paths(fake_devfreq, fake_devfreq_symdir, dev);
  }

  EXPECT_CALL(*data_source, OpenDirAndLogOnErrorOnce(
                                "/sys/class/devfreq/",
                                data_source->GetDevfreqErrorLoggedAddress()))
      .WillRepeatedly(Invoke([&fake_devfreq_symdir] {
        return base::ScopedDir(opendir(fake_devfreq_symdir.path().c_str()));
      }));
  EXPECT_CALL(*data_source, ReadDevfreqCurFreq("10010.devfreq_device_a"))
      .WillRepeatedly(Return(kDevfreq1));
  EXPECT_CALL(*data_source, ReadDevfreqCurFreq("10020.devfreq_device_b"))
      .WillRepeatedly(Return(kDevfreq2));

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.meminfo_size(), 0);
  EXPECT_EQ(sys_stats.cpu_stat_size(), 0);

  using KV = std::pair<std::string, uint64_t>;
  std::vector<KV> kvs;
  for (const auto& kv : sys_stats.devfreq())
    kvs.push_back({kv.key(), kv.value()});
  EXPECT_THAT(kvs,
              UnorderedElementsAre(KV{"10010.devfreq_device_a", 1000000},
                                   KV{"10020.devfreq_device_b", 20000000}));
  for (const std::string& path : dirs_to_delete)
    base::Rmdir(path);
  for (const std::string& path : symlinks_to_delete)
    remove(path.c_str());
}

TEST_F(SysStatsDataSourceTest, StatAll) {
  DataSourceConfig config;
  protos::gen::SysStatsConfig sys_cfg;
  sys_cfg.set_stat_period_ms(10);
  config.set_sys_stats_config_raw(sys_cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.meminfo_size(), 0);
  EXPECT_EQ(sys_stats.vmstat_size(), 0);
  EXPECT_EQ(sys_stats.buddy_info_size(), 0);

  ASSERT_EQ(sys_stats.cpu_stat_size(), 8);
  EXPECT_EQ(sys_stats.cpu_stat()[0].user_ns(), 762178 * 10000000ull);
  EXPECT_EQ(sys_stats.cpu_stat()[0].system_mode_ns(), 902284 * 10000000ull);
  EXPECT_EQ(sys_stats.cpu_stat()[0].softirq_ns(), 68262 * 10000000ull);
  EXPECT_EQ(sys_stats.cpu_stat()[0].steal_ns(), 72386 * 10000000ull);
  EXPECT_EQ(sys_stats.cpu_stat()[7].user_ns(), 180484 * 10000000ull);
  EXPECT_EQ(sys_stats.cpu_stat()[7].system_mode_ns(), 139874 * 10000000ull);
  EXPECT_EQ(sys_stats.cpu_stat()[7].softirq_ns(), 13407 * 10000000ull);
  EXPECT_EQ(sys_stats.cpu_stat()[7].steal_ns(), 28643 * 10000000ull);

  EXPECT_EQ(sys_stats.num_forks(), 243320u);

  EXPECT_EQ(sys_stats.num_irq_total(), 238128517u);
  ASSERT_EQ(sys_stats.num_irq_size(), 102);
  EXPECT_EQ(sys_stats.num_irq()[0].count(), 63500984u);
  EXPECT_EQ(sys_stats.num_irq()[0].irq(), 3);
  EXPECT_EQ(sys_stats.num_irq()[1].count(), 6253792u);
  EXPECT_EQ(sys_stats.num_irq()[1].irq(), 5);
  EXPECT_EQ(sys_stats.num_irq()[101].count(), 680u);

  EXPECT_EQ(sys_stats.num_softirq_total(), 84611084u);
  ASSERT_EQ(sys_stats.num_softirq_size(), 10);
  EXPECT_EQ(sys_stats.num_softirq()[0].count(), 10220177u);
  EXPECT_EQ(sys_stats.num_softirq()[9].count(), 16443195u);

  EXPECT_EQ(sys_stats.num_softirq_total(), 84611084u);
}

TEST_F(SysStatsDataSourceTest, StatForksOnly) {
  protos::gen::SysStatsConfig cfg;
  cfg.set_stat_period_ms(10);
  cfg.add_stat_counters(protos::gen::SysStatsConfig::STAT_FORK_COUNT);
  DataSourceConfig config_obj;
  config_obj.set_sys_stats_config_raw(cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config_obj);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.meminfo_size(), 0);
  EXPECT_EQ(sys_stats.vmstat_size(), 0);
  EXPECT_EQ(sys_stats.buddy_info_size(), 0);
  ASSERT_EQ(sys_stats.cpu_stat_size(), 0);
  EXPECT_EQ(sys_stats.num_forks(), 243320u);
  EXPECT_EQ(sys_stats.num_irq_total(), 0u);
  ASSERT_EQ(sys_stats.num_irq_size(), 0);
  EXPECT_EQ(sys_stats.num_softirq_total(), 0u);
  ASSERT_EQ(sys_stats.num_softirq_size(), 0);
}

TEST_F(SysStatsDataSourceTest, Cpufreq) {
  protos::gen::SysStatsConfig cfg;
  cfg.set_cpufreq_period_ms(10);
  DataSourceConfig config_obj;
  config_obj.set_sys_stats_config_raw(cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config_obj);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_GT(sys_stats.cpufreq_khz_size(), 0);
  EXPECT_EQ(sys_stats.cpufreq_khz()[0], 2650000u);
  if (sys_stats.cpufreq_khz_size() > 1) {
    // We emulated 2 CPUs but it is possible the test system is single core.
    EXPECT_EQ(sys_stats.cpufreq_khz()[1], 3698200u);
  }
  for (unsigned int i = 2;
       i < static_cast<unsigned int>(sys_stats.cpufreq_khz_size()); i++) {
    // For cpux which scaling_cur_freq was not emulated in unittest, cpufreq
    // should be recorded as 0
    EXPECT_EQ(sys_stats.cpufreq_khz()[i], 0u);
  }
}

TEST_F(SysStatsDataSourceTest, DiskStat) {
  protos::gen::SysStatsConfig cfg;
  cfg.set_diskstat_period_ms(10);
  DataSourceConfig config_obj;
  config_obj.set_sys_stats_config_raw(cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config_obj);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  EXPECT_EQ(sys_stats.disk_stat_size(), 3);

  EXPECT_EQ(sys_stats.disk_stat()[0].device_name(), "zram0");
  EXPECT_EQ(sys_stats.disk_stat()[0].read_sectors(), 111088u);
  EXPECT_EQ(sys_stats.disk_stat()[0].write_sectors(), 458384u);
  EXPECT_EQ(sys_stats.disk_stat()[0].discard_sectors(), 0u);
  EXPECT_EQ(sys_stats.disk_stat()[0].flush_count(), 0u);
  EXPECT_EQ(sys_stats.disk_stat()[0].read_time_ms(), 128u);
  EXPECT_EQ(sys_stats.disk_stat()[0].write_time_ms(), 48u);
  EXPECT_EQ(sys_stats.disk_stat()[0].discard_time_ms(), 0u);
  EXPECT_EQ(sys_stats.disk_stat()[0].flush_time_ms(), 0u);
  EXPECT_EQ(sys_stats.disk_stat()[1].device_name(), "sda");
  EXPECT_EQ(sys_stats.disk_stat()[1].read_sectors(), 8221736u);
  EXPECT_EQ(sys_stats.disk_stat()[1].write_sectors(), 9599744u);
  EXPECT_EQ(sys_stats.disk_stat()[1].discard_sectors(), 2160072u);
  EXPECT_EQ(sys_stats.disk_stat()[1].flush_count(), 6649u);
  EXPECT_EQ(sys_stats.disk_stat()[1].read_time_ms(), 75929u);
  EXPECT_EQ(sys_stats.disk_stat()[1].write_time_ms(), 143190u);
  EXPECT_EQ(sys_stats.disk_stat()[1].discard_time_ms(), 19411u);
  EXPECT_EQ(sys_stats.disk_stat()[1].flush_time_ms(), 11327u);
  EXPECT_EQ(sys_stats.disk_stat()[2].device_name(), "sda1");
  EXPECT_EQ(sys_stats.disk_stat()[2].read_sectors(), 632u);
  EXPECT_EQ(sys_stats.disk_stat()[2].write_sectors(), 704u);
  EXPECT_EQ(sys_stats.disk_stat()[2].discard_sectors(), 0u);
  EXPECT_EQ(sys_stats.disk_stat()[2].flush_count(), 0u);
  EXPECT_EQ(sys_stats.disk_stat()[2].read_time_ms(), 7u);
  EXPECT_EQ(sys_stats.disk_stat()[2].write_time_ms(), 92u);
  EXPECT_EQ(sys_stats.disk_stat()[2].discard_time_ms(), 0u);
  EXPECT_EQ(sys_stats.disk_stat()[2].flush_time_ms(), 0u);
}

TEST_F(SysStatsDataSourceTest, Psi) {
  protos::gen::SysStatsConfig cfg;
  cfg.set_psi_period_ms(10);
  DataSourceConfig config_obj;
  config_obj.set_sys_stats_config_raw(cfg.SerializeAsString());
  auto data_source = GetSysStatsDataSource(config_obj);

  WaitTick(data_source.get());

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_sys_stats());
  const auto& sys_stats = packet.sys_stats();
  ASSERT_EQ(sys_stats.psi_size(), 6);

  using PsiSample = protos::gen::SysStats::PsiSample;
  EXPECT_EQ(sys_stats.psi()[0].resource(), PsiSample::PSI_RESOURCE_CPU_SOME);
  EXPECT_EQ(sys_stats.psi()[0].total_ns(), 417963000u);
  EXPECT_EQ(sys_stats.psi()[1].resource(), PsiSample::PSI_RESOURCE_CPU_FULL);
  EXPECT_EQ(sys_stats.psi()[1].total_ns(), 205933000U);
  EXPECT_EQ(sys_stats.psi()[2].resource(), PsiSample::PSI_RESOURCE_IO_SOME);
  EXPECT_EQ(sys_stats.psi()[2].total_ns(), 417963000u);
  EXPECT_EQ(sys_stats.psi()[3].resource(), PsiSample::PSI_RESOURCE_IO_FULL);
  EXPECT_EQ(sys_stats.psi()[3].total_ns(), 205933000U);
  EXPECT_EQ(sys_stats.psi()[4].resource(), PsiSample::PSI_RESOURCE_MEMORY_SOME);
  EXPECT_EQ(sys_stats.psi()[4].total_ns(), 417963000u);
  EXPECT_EQ(sys_stats.psi()[5].resource(), PsiSample::PSI_RESOURCE_MEMORY_FULL);
  EXPECT_EQ(sys_stats.psi()[5].total_ns(), 205933000U);
}

}  // namespace
}  // namespace perfetto
