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

#include "src/traced/probes/common/cpu_freq_info_for_testing.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include "test/gtest_and_gmock.h"

using ::testing::ElementsAre;

namespace perfetto {
namespace {

class CpuFreqInfoTest : public ::testing::Test {
 protected:
  std::unique_ptr<CpuFreqInfo> GetCpuFreqInfo() {
    return std::unique_ptr<CpuFreqInfo>(
        cpu_freq_info_for_testing.GetInstance());
  }

  CpuFreqInfoForTesting cpu_freq_info_for_testing;
};

std::vector<uint32_t> FreqsToVector(CpuFreqInfo::Range range) {
  std::vector<uint32_t> freqs;
  for (auto it = range.first; it != range.second; it++)
    freqs.push_back(*it);
  return freqs;
}

TEST_F(CpuFreqInfoTest, CpuFreqInfo) {
  auto cpu_freq_info = GetCpuFreqInfo();

  EXPECT_THAT(FreqsToVector(cpu_freq_info->GetFreqs(0u)),
              ElementsAre(300000, 576000, 748800, 998400, 1209600, 1324800,
                          1516800, 1612800, 1708800));
  EXPECT_THAT(FreqsToVector(cpu_freq_info->GetFreqs(1u)),
              ElementsAre(300000, 652800, 825600, 979200, 1132800, 1363200,
                          1536000, 1747200, 1843200, 1996800, 2803200));
  EXPECT_THAT(FreqsToVector(cpu_freq_info->GetFreqs(2u)), ElementsAre());
  EXPECT_THAT(FreqsToVector(cpu_freq_info->GetFreqs(100u)), ElementsAre());

  EXPECT_EQ(cpu_freq_info->GetCpuFreqIndex(0u, 300000u), 1u);
  EXPECT_EQ(cpu_freq_info->GetCpuFreqIndex(0u, 748800u), 3u);
  EXPECT_EQ(cpu_freq_info->GetCpuFreqIndex(1u, 300000u), 10u);
  EXPECT_EQ(cpu_freq_info->GetCpuFreqIndex(1u, 1996800u), 19u);
  EXPECT_EQ(cpu_freq_info->GetCpuFreqIndex(1u, 2803200), 20u);
  EXPECT_EQ(cpu_freq_info->GetCpuFreqIndex(1u, 5u), 0u);
}

TEST_F(CpuFreqInfoTest, ReadCpuCurrFreqPersistentAndRecovery) {
  auto cpu_freq_info = GetCpuFreqInfo();
  std::string cpu0_path =
      cpu_freq_info_for_testing.AbsolutePath("cpu0/cpufreq/scaling_cur_freq");

  auto write_file = [](const std::string& path, const char* data, int flags) {
    int fd = open(path.c_str(), flags, 0600);
    ASSERT_GE(fd, 0);
    size_t len = strlen(data);
    if (len > 0) {
      ASSERT_EQ(write(fd, data, len), static_cast<ssize_t>(len));
    }
    ASSERT_EQ(close(fd), 0);
  };

  const auto& first = cpu_freq_info->ReadCpuCurrFreq();
  ASSERT_GE(first.size(), 1u);
  EXPECT_EQ(first[0], 2650000u);
  if (first.size() >= 2u) {
    EXPECT_EQ(first[1], 3698200u);
  }

  // Overwrite in place (same inode): persistent FD observes updated frequency.
  write_file(cpu0_path, "1800000\n", O_WRONLY | O_TRUNC);
  EXPECT_EQ(cpu_freq_info->ReadCpuCurrFreq()[0], 1800000u);

  // Empty read resets cached FD so a recreated file (new inode, e.g. hotplug)
  // is re-opened on a subsequent poll.
  write_file(cpu0_path, "", O_WRONLY | O_TRUNC);
  EXPECT_EQ(cpu_freq_info->ReadCpuCurrFreq()[0], 0u);

  ASSERT_EQ(unlink(cpu0_path.c_str()), 0);
  write_file(cpu0_path, "2400000", O_WRONLY | O_CREAT);
  EXPECT_EQ(cpu_freq_info->ReadCpuCurrFreq()[0], 2400000u);

  // Non-numeric contents report 0 without crashing.
  write_file(cpu0_path, "invalid\n", O_WRONLY | O_TRUNC);
  EXPECT_EQ(cpu_freq_info->ReadCpuCurrFreq()[0], 0u);
}

}  // namespace
}  // namespace perfetto
