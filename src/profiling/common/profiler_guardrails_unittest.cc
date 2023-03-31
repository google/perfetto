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

#include <unistd.h>

#include <cinttypes>
#include <map>
#include <optional>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

TEST(ProfilerCpuGuardrailsTest, Exceeded) {
  const auto clk = static_cast<unsigned long>(sysconf(_SC_CLK_TCK));
  base::TempFile f = base::TempFile::CreateUnlinked();
  constexpr const char stat[] =
      "2965981 (zsh) S 2965977 2965981 2965981 34822 2966607 4194304 6632 6697 "
      "0 0 1000000 6000000 4 1 20 0 1 0 227163466 15839232 2311 "
      "18446744073709551615 "
      "94823961161728 94823961762781 140722993535472 0 0 0 2 3686400 134295555 "
      "0 0 0 17 2 0 0 0 0 0 94823961905904 94823961935208 94823993954304 "
      "140722993543678 140722993543691 140722993543691 140722993545195 0";
  base::WriteAll(f.fd(), stat, sizeof(stat));
  ASSERT_NE(lseek(f.fd(), 0, SEEK_SET), -1);
  ProfilerCpuGuardrails gr(f.ReleaseFD());

  GuardrailConfig gc;
  gc.cpu_guardrail_sec = 5000000 / clk;
  gc.cpu_start_secs = 1000000 / clk;
  EXPECT_TRUE(gr.IsOverCpuThreshold(gc));
}

TEST(ProfilerCpuGuardrailsTest, NotExceeded) {
  const auto clk = static_cast<unsigned long>(sysconf(_SC_CLK_TCK));
  base::TempFile f = base::TempFile::CreateUnlinked();
  constexpr const char stat[] =
      "2965981 (zsh) S 2965977 2965981 2965981 34822 2966607 4194304 6632 6697 "
      "0 0 1000000 6000000 4 1 20 0 1 0 227163466 15839232 2311 "
      "18446744073709551615 "
      "94823961161728 94823961762781 140722993535472 0 0 0 2 3686400 134295555 "
      "0 0 0 17 2 0 0 0 0 0 94823961905904 94823961935208 94823993954304 "
      "140722993543678 140722993543691 140722993543691 140722993545195 0";
  base::WriteAll(f.fd(), stat, sizeof(stat));
  ASSERT_NE(lseek(f.fd(), 0, SEEK_SET), -1);
  ProfilerCpuGuardrails gr(f.ReleaseFD());

  GuardrailConfig gc;
  gc.cpu_guardrail_sec = 7000000 / clk;
  gc.cpu_start_secs = 1000000 / clk;
  EXPECT_FALSE(gr.IsOverCpuThreshold(gc));
}

TEST(ProfilerMemoryGuardrailsTest, Exceeded) {
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
  ASSERT_NE(lseek(f.fd(), 0, SEEK_SET), -1);
  ProfilerMemoryGuardrails gr(f.ReleaseFD());

  GuardrailConfig gc;
  gc.memory_guardrail_kb = 77;
  EXPECT_TRUE(gr.IsOverMemoryThreshold(gc));
}

TEST(ProfilerMemoryGuardrailsTest, NotExceeded) {
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
  ASSERT_NE(lseek(f.fd(), 0, SEEK_SET), -1);
  ProfilerMemoryGuardrails gr(f.ReleaseFD());
  GuardrailConfig gc;
  gc.memory_guardrail_kb = 100;
  EXPECT_FALSE(gr.IsOverMemoryThreshold(gc));
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
