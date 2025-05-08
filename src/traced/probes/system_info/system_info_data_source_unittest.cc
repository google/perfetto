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

#include "src/traced/probes/system_info/system_info_data_source.h"
#include "src/traced/probes/common/cpu_freq_info_for_testing.h"
#include "src/traced/probes/system_info/cpu_info_features_allowlist.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/system_info/cpu_info.gen.h"

using ::testing::AnyOf;
using ::testing::ElementsAre;
using ::testing::Return;

namespace perfetto {
namespace {

static const uint32_t CPU_COUNT = 8;

const char kMockCpuInfoAndroid[] = R"(
Processor	: AArch64 Processor rev 13 (aarch64)
processor	: 0
BogoMIPS	: 38.00
Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp
CPU implementer	: 0x51
CPU architecture: 8
CPU variant	: 0x7
CPU part	: 0x803
CPU revision	: 12

processor	: 1
BogoMIPS	: 38.00
Features	: fp mte mte3
CPU implementer	: 0x51
CPU architecture: 8
CPU variant	: 0x7
CPU part	: 0x803
CPU revision	: 12

processor	: 2
BogoMIPS	: 38.00
Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp
CPU implementer	: 0x51
CPU architecture: 8
CPU variant	: 0x7
CPU part	: 0x803
CPU revision	: 12

processor	: 3
BogoMIPS	: 38.00
Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp
CPU implementer	: 0x51
CPU architecture: 8
CPU variant	: 0x7
CPU part	: 0x803
CPU revision	: 12

processor	: 4
BogoMIPS	: 38.00
Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp
CPU implementer	: 0x51
CPU architecture: 8
CPU variant	: 0x7
CPU part	: 0x803
CPU revision	: 12

processor	: 5
BogoMIPS	: 38.00
Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp
CPU implementer	: 0x51
CPU architecture: 8
CPU variant	: 0x7
CPU part	: 0x803
CPU revision	: 12

processor	: 6
BogoMIPS	: 38.00
Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp
CPU implementer	: 0x51
CPU architecture: 8
CPU variant	: 0x6
CPU part	: 0x802
CPU revision	: 13

processor	: 7
BogoMIPS	: 38.00
Features	: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp
CPU implementer	: 0x51
CPU architecture: 8
CPU variant	: 0x6
CPU part	: 0x802
CPU revision	: 13

Hardware	: Qualcomm Technologies, Inc SDM670

)";

const char* kMockCpuCapacityInfoAndroid[8] = {
    "200\n", "200\n", "200\n", "600\n", "600\n", "600\n", "1024\n", "1024\n"};

class TestSystemInfoDataSource : public SystemInfoDataSource {
 public:
  TestSystemInfoDataSource(std::unique_ptr<TraceWriter> writer,
                           std::unique_ptr<CpuFreqInfo> cpu_freq_info)
      : SystemInfoDataSource(
            /* session_id */ 0,
            std::move(writer),
            std::move(cpu_freq_info)) {}

  MOCK_METHOD(std::string, ReadFile, (std::string), (override));
};

class SystemInfoDataSourceTest : public ::testing::Test {
 protected:
  std::unique_ptr<TestSystemInfoDataSource> GetSystemInfoDataSource() {
    auto writer =
        std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
    writer_raw_ = writer.get();
    auto instance =
        std::unique_ptr<TestSystemInfoDataSource>(new TestSystemInfoDataSource(
            std::move(writer), cpu_freq_info_for_testing.GetInstance()));
    return instance;
  }

  TraceWriterForTesting* writer_raw_ = nullptr;
  CpuFreqInfoForTesting cpu_freq_info_for_testing;
};

TEST_F(SystemInfoDataSourceTest, CpuInfoAndroid) {
  auto data_source = GetSystemInfoDataSource();
  EXPECT_CALL(*data_source, ReadFile("/proc/cpuinfo"))
      .WillOnce(Return(kMockCpuInfoAndroid));

  for (uint32_t cpu_index = 0; cpu_index < CPU_COUNT; cpu_index++) {
    EXPECT_CALL(*data_source,
                ReadFile("/sys/devices/system/cpu/cpu" +
                         std::to_string(cpu_index) + "/cpu_capacity"))
        .WillOnce(Return(kMockCpuCapacityInfoAndroid[cpu_index]));
  }

  data_source->Start();

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_cpu_info());
  auto cpu_info = packet.cpu_info();
  ASSERT_EQ(cpu_info.cpus_size(), 8);
  auto cpu = cpu_info.cpus()[0];
  ASSERT_EQ(cpu.processor(), "AArch64 Processor rev 13 (aarch64)");
  ASSERT_THAT(cpu.frequencies(),
              ElementsAre(300000, 576000, 748800, 998400, 1209600, 1324800,
                          1516800, 1612800, 1708800));
  ASSERT_TRUE(cpu.has_arm_identifier());
  auto id = cpu.arm_identifier();
  ASSERT_EQ(id.implementer(), 0x51U);
  ASSERT_EQ(id.architecture(), 8U);
  ASSERT_EQ(id.variant(), 0x7U);
  ASSERT_EQ(id.part(), 0x803U);
  ASSERT_EQ(id.revision(), 12U);

  ASSERT_EQ(cpu.capacity(), static_cast<uint32_t>(200));
  cpu = cpu_info.cpus()[1];
  ASSERT_EQ(cpu.processor(), "AArch64 Processor rev 13 (aarch64)");
  ASSERT_THAT(cpu.frequencies(),
              ElementsAre(300000, 652800, 825600, 979200, 1132800, 1363200,
                          1536000, 1747200, 1843200, 1996800, 2803200));
  ASSERT_TRUE(cpu.has_arm_identifier());
  id = cpu.arm_identifier();
  ASSERT_EQ(id.implementer(), 0x51U);
  ASSERT_EQ(id.architecture(), 8U);
  ASSERT_EQ(id.variant(), 0x7U);
  ASSERT_EQ(id.part(), 0x803U);
  ASSERT_EQ(id.revision(), 12U);
  ASSERT_TRUE(cpu.features() & (1u << 0));
  ASSERT_STREQ(kCpuInfoFeatures[0], "mte");
  ASSERT_TRUE(cpu.features() & (1u << 1));
  ASSERT_STREQ(kCpuInfoFeatures[1], "mte3");

  cpu = cpu_info.cpus()[7];
  ASSERT_EQ(cpu.capacity(), static_cast<uint32_t>(1024));
  ASSERT_TRUE(cpu.has_arm_identifier());
  id = cpu.arm_identifier();
  ASSERT_EQ(id.implementer(), 0x51U);
  ASSERT_EQ(id.architecture(), 8U);
  ASSERT_EQ(id.variant(), 0x6U);
  ASSERT_EQ(id.part(), 0x802U);
  ASSERT_EQ(id.revision(), 13U);
  ASSERT_EQ(cpu.features(), 0U);
}

}  // namespace
}  // namespace perfetto
