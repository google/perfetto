/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/profiling/common/profiler_guardrails.h"

#include <inttypes.h>
#include <unistd.h>

#include <map>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

class StubDataSource {
 public:
  StubDataSource(uint64_t cpu_guardrails_secs,
                 uint64_t cpu_start_secs,
                 uint32_t memory_guardrail_kb)
      : cpu_guardrails_secs_(cpu_guardrails_secs),
        cpu_start_secs_(cpu_start_secs),
        memory_guardrail_kb_(memory_guardrail_kb) {}

  uint64_t GetCpuGuardrailSecs() const { return cpu_guardrails_secs_; }

  base::Optional<uint64_t> GetCpuStartSecs() const { return cpu_start_secs_; }

  uint32_t GetMemoryGuardrailKb() const { return memory_guardrail_kb_; }

  void Delete() { deleted_ = true; }
  bool deleted() { return deleted_; }

 private:
  uint64_t cpu_guardrails_secs_;
  uint64_t cpu_start_secs_;
  uint32_t memory_guardrail_kb_;
  bool deleted_ = false;
};

TEST(ProfilerCpuGuardrailsTest, Exceeded) {
  const auto clk = static_cast<unsigned long>(sysconf(_SC_CLK_TCK));
  StubDataSource ds(5000000 / clk, 1000000 / clk, 0);
  std::map<int, StubDataSource> ds_map = {{1, std::move(ds)}};
  base::TempFile f = base::TempFile::CreateUnlinked();
  constexpr const char stat[] =
      "2965981 (zsh) S 2965977 2965981 2965981 34822 2966607 4194304 6632 6697 "
      "0 0 1000000 6000000 4 1 20 0 1 0 227163466 15839232 2311 "
      "18446744073709551615 "
      "94823961161728 94823961762781 140722993535472 0 0 0 2 3686400 134295555 "
      "0 0 0 17 2 0 0 0 0 0 94823961905904 94823961935208 94823993954304 "
      "140722993543678 140722993543691 140722993543691 140722993545195 0";
  base::WriteAll(f.fd(), stat, sizeof(stat));
  ProfilerCpuGuardrails gr(f.ReleaseFD());
  gr.CheckDataSourceCpu(ds_map.begin(), ds_map.end(),
                        [](StubDataSource* d) { d->Delete(); });
  auto it = ds_map.find(1);
  ASSERT_NE(it, ds_map.end());
  EXPECT_TRUE(it->second.deleted());
}

TEST(ProfilerCpuGuardrailsTest, NotExceeded) {
  const auto clk = static_cast<unsigned long>(sysconf(_SC_CLK_TCK));
  StubDataSource ds(7000000 / clk, 1000000 / clk, 0);
  std::map<int, StubDataSource> ds_map = {{1, std::move(ds)}};
  base::TempFile f = base::TempFile::CreateUnlinked();
  constexpr const char stat[] =
      "2965981 (zsh) S 2965977 2965981 2965981 34822 2966607 4194304 6632 6697 "
      "0 0 1000000 6000000 4 1 20 0 1 0 227163466 15839232 2311 "
      "18446744073709551615 "
      "94823961161728 94823961762781 140722993535472 0 0 0 2 3686400 134295555 "
      "0 0 0 17 2 0 0 0 0 0 94823961905904 94823961935208 94823993954304 "
      "140722993543678 140722993543691 140722993543691 140722993545195 0";
  base::WriteAll(f.fd(), stat, sizeof(stat));
  ProfilerCpuGuardrails gr(f.ReleaseFD());
  gr.CheckDataSourceCpu(ds_map.begin(), ds_map.end(),
                        [](StubDataSource* d) { d->Delete(); });
  auto it = ds_map.find(1);
  ASSERT_NE(it, ds_map.end());
  EXPECT_FALSE(it->second.deleted());
}

TEST(ProfilerMemoryGuardrailsTest, Exceeded) {
  StubDataSource ds(0, 0, 77);
  std::map<int, StubDataSource> ds_map = {{1, std::move(ds)}};
  base::TempFile f = base::TempFile::CreateUnlinked();
  constexpr const char status[] =
      "VmPeak:\t    5432 kB\n"
      "VmSize:\t    5432 kB\n"
      "VmLck:\t       0 kB\n"
      "VmPin:\t       0 kB\n"
      "VmHWM:\t     584 kB\n"
      "VmRSS:\t     80 kB\n"
      "RssAnon:\t      68 kB\n"
      "RssFile:\t     516 kB\n"
      "RssShmem:\t       0 kB\n"
      "VmData:\t     316 kB\n"
      "VmStk:\t     132 kB\n"
      "VmExe:\t      20 kB\n"
      "VmLib:\t    1460 kB\n"
      "VmPTE:\t      44 kB\n"
      "VmSwap:\t       10 kB\n";

  base::WriteAll(f.fd(), status, sizeof(status));
  ProfilerMemoryGuardrails gr(f.ReleaseFD());
  gr.CheckDataSourceMemory(ds_map.begin(), ds_map.end(),
                           [](StubDataSource* d) { d->Delete(); });
  auto it = ds_map.find(1);
  ASSERT_NE(it, ds_map.end());
  EXPECT_TRUE(it->second.deleted());
}

TEST(ProfilerMemoryGuardrailsTest, NotExceeded) {
  StubDataSource ds(0, 0, 100);
  std::map<int, StubDataSource> ds_map = {{1, std::move(ds)}};
  base::TempFile f = base::TempFile::CreateUnlinked();
  constexpr const char status[] =
      "VmPeak:\t    5432 kB\n"
      "VmSize:\t    5432 kB\n"
      "VmLck:\t       0 kB\n"
      "VmPin:\t       0 kB\n"
      "VmHWM:\t     584 kB\n"
      "VmRSS:\t     80 kB\n"
      "RssAnon:\t      68 kB\n"
      "RssFile:\t     516 kB\n"
      "RssShmem:\t       0 kB\n"
      "VmData:\t     316 kB\n"
      "VmStk:\t     132 kB\n"
      "VmExe:\t      20 kB\n"
      "VmLib:\t    1460 kB\n"
      "VmPTE:\t      44 kB\n"
      "VmSwap:\t       10 kB\n";

  base::WriteAll(f.fd(), status, sizeof(status));
  ProfilerMemoryGuardrails gr(f.ReleaseFD());
  gr.CheckDataSourceMemory(ds_map.begin(), ds_map.end(),
                           [](StubDataSource* d) { d->Delete(); });
  auto it = ds_map.find(1);
  ASSERT_NE(it, ds_map.end());
  EXPECT_FALSE(it->second.deleted());
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
