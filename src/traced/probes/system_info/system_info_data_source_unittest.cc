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
#include "perfetto/ext/base/cpu_info_features_allowlist.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/traced/probes/common/cpu_freq_info_for_testing.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/config/system_info/system_info_config.gen.h"
#include "protos/perfetto/trace/system_info/cpu_info.gen.h"
#include "protos/perfetto/trace/system_info/interrupt_info.gen.h"

using ::testing::AnyOf;
using ::testing::ElementsAre;
using ::testing::Return;

namespace perfetto {
namespace {

static const uint32_t CPU_COUNT = 8;

// ARM (Android) /proc/interrupts with 7 CPUs. Covers:
//   - GICv3 standalone Level trigger
//   - Edge trigger
//   - Multi-word name after trigger (irq 701: "cs40l26 IRQ1 Controller")
//   - Multi-word chip name with Level trigger (irq 702: "GPIO1 rise")
//   - PCI-MSI with standalone Edge (irq 762)
//   - Unknown trigger → last-token fallback (irq 999: "fallback_device")
//   - Too few tokens: only IRQ_NUM + CPU counts (irq 888, skipped)
//   - Too few tokens: controller only, no name (irq 889, skipped)
//   - Non-numeric IPI row (skipped)
const char kArmProcInterrupts[] =
    "           CPU0       CPU1       CPU2       CPU3       CPU4       CPU5    "
    "   CPU6       \n"
    "  9:          0          0          0          0          0          0    "
    "      0     GICv3  25 Level     vgic\n"
    " 11:    4083471    3838684     164581     132724     127805     125978    "
    "  74543     GICv3  30 Level     arch_timer\n"
    "700:          4         48          0          0          0          0    "
    "      0  3c280000.pinctrl   1 Edge      u100_power_state\n"
    "701:         75          0          0          0          0          0    "
    "      0  e500000.pinctrl  63 Level     cs40l26 IRQ1 Controller\n"
    "702:          0          0          0          0          0          0    "
    "      0  cs40l26 IRQ1 Controller   0 Level     GPIO1 rise\n"
    "762:          0          0          0          0          0          0    "
    "      0   PCI-MSI 524319 Edge      msix31-01000\n"
    "999:          0          0          0          0          0          0    "
    "      0  CUSTOM-CTRL  42  fallback_device\n"
    "888:          0          0          0          0          0          0    "
    "      0\n"
    "889:          0          0          0          0          0          0    "
    "      0  GIC-400\n"
    "IPI0:   1976106    2709737     366791     328045     337128     402772    "
    " 148385       Rescheduling interrupts\n";

// x86 /proc/interrupts with 4 CPUs. Covers:
//   - IO-APIC with -edge embedded trigger
//   - IO-APIC with -fasteoi embedded trigger
//   - IR-PCI-MSI with -edge and multi-word name
//   - Unknown trigger → last-token fallback (irq 200: "fallback_device")
//   - Too few tokens: only IRQ_NUM + CPU counts (irq 888, skipped)
//   - Too few tokens: controller only, no name (irq 889, skipped)
//   - Non-numeric rows NMI/LOC (skipped)
const char kX86ProcInterrupts[] =
    "           CPU0       CPU1       CPU2       CPU3       \n"
    "   0:         42          0          0          0 IR-IO-APIC    2-edge    "
    "  timer\n"
    "   7:          0          0          0          0 IR-IO-APIC    7-fasteoi "
    "  pinctrl_amd\n"
    "  31:          0          0          0          1 IR-PCI-MSI-0000:60:03.1 "
    "   0-edge      PCIe PME, PCIe bwctrl\n"
    " 200:          0          0          0          0  CUSTOM-CTRL  42  "
    "fallback_device\n"
    " 888:          0          0          0          0\n"
    " 889:          0          0          0          0  CTRL\n"
    "NMI:      12968       4678       3377       3532   Non-maskable "
    "interrupts\n"
    "LOC:  470006507  556112258  531573082  543196117   Local timer "
    "interrupts\n";

DataSourceConfig MakeIrqNamesConfig() {
  DataSourceConfig config;
  config.mutable_system_info_config()->set_irq_names(true);
  return config;
}

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
                           std::unique_ptr<CpuFreqInfo> cpu_freq_info,
                           const DataSourceConfig& config)
      : SystemInfoDataSource(0,
                             std::move(writer),
                             std::move(cpu_freq_info),
                             config) {}

  MOCK_METHOD(std::vector<base::CpuInfo>, ReadCpuInfo, (), (override));
  MOCK_METHOD(std::string, ReadFile, (std::string), (override));
};

class SystemInfoDataSourceTest : public ::testing::Test {
 protected:
  std::unique_ptr<TestSystemInfoDataSource> GetSystemInfoDataSource(
      const DataSourceConfig& config = DataSourceConfig()) {
    auto writer =
        std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
    writer_raw_ = writer.get();
    auto instance =
        std::unique_ptr<TestSystemInfoDataSource>(new TestSystemInfoDataSource(
            std::move(writer), cpu_freq_info_for_testing.GetInstance(),
            config));
    return instance;
  }

  TraceWriterForTesting* writer_raw_ = nullptr;
  CpuFreqInfoForTesting cpu_freq_info_for_testing;
};

TEST_F(SystemInfoDataSourceTest, CpuInfoAndroid) {
  auto data_source = GetSystemInfoDataSource();
  EXPECT_CALL(*data_source, ReadCpuInfo())
      .WillOnce(Return(base::ParseCpuInfo(kMockCpuInfoAndroid)));

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
  ASSERT_STREQ(base::kCpuInfoFeatures[0], "mte");
  ASSERT_TRUE(cpu.features() & (1u << 1));
  ASSERT_STREQ(base::kCpuInfoFeatures[1], "mte3");

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

TEST_F(SystemInfoDataSourceTest, IrqMappingArm) {
  auto data_source = GetSystemInfoDataSource(MakeIrqNamesConfig());
  EXPECT_CALL(*data_source, ReadCpuInfo())
      .WillOnce(Return(std::vector<base::CpuInfo>{}));
  EXPECT_CALL(*data_source, ReadFile("/proc/interrupts"))
      .WillOnce(Return(std::string(kArmProcInterrupts)));

  data_source->Start();

  auto packets = writer_raw_->GetAllTracePackets();
  const protos::gen::InterruptInfo* irq_info = nullptr;
  for (const auto& p : packets) {
    if (p.has_interrupt_info()) {
      irq_info = &p.interrupt_info();
      break;
    }
  }
  ASSERT_NE(irq_info, nullptr);
  // 7 numeric IRQs; IPI0 is non-numeric and must be skipped.
  ASSERT_EQ(irq_info->irq_mapping_size(), 7);

  // irq 9: GICv3 standalone Level
  EXPECT_EQ(irq_info->irq_mapping()[0].irq_id(), 9u);
  EXPECT_EQ(irq_info->irq_mapping()[0].name(), "vgic");
  // irq 11: GICv3 standalone Level
  EXPECT_EQ(irq_info->irq_mapping()[1].irq_id(), 11u);
  EXPECT_EQ(irq_info->irq_mapping()[1].name(), "arch_timer");
  // irq 700: Edge trigger
  EXPECT_EQ(irq_info->irq_mapping()[2].irq_id(), 700u);
  EXPECT_EQ(irq_info->irq_mapping()[2].name(), "u100_power_state");
  // irq 701: multi-word name after Level trigger
  EXPECT_EQ(irq_info->irq_mapping()[3].irq_id(), 701u);
  EXPECT_EQ(irq_info->irq_mapping()[3].name(), "cs40l26 IRQ1 Controller");
  // irq 702: multi-word chip name; anchor search finds Level despite variable
  // offset
  EXPECT_EQ(irq_info->irq_mapping()[4].irq_id(), 702u);
  EXPECT_EQ(irq_info->irq_mapping()[4].name(), "GPIO1 rise");
  // irq 762: PCI-MSI with standalone Edge
  EXPECT_EQ(irq_info->irq_mapping()[5].irq_id(), 762u);
  EXPECT_EQ(irq_info->irq_mapping()[5].name(), "msix31-01000");
  // irq 999: no recognised trigger → last-token fallback
  EXPECT_EQ(irq_info->irq_mapping()[6].irq_id(), 999u);
  EXPECT_EQ(irq_info->irq_mapping()[6].name(), "fallback_device");
}

TEST_F(SystemInfoDataSourceTest, IrqMappingX86) {
  auto data_source = GetSystemInfoDataSource(MakeIrqNamesConfig());
  EXPECT_CALL(*data_source, ReadCpuInfo())
      .WillOnce(Return(std::vector<base::CpuInfo>{}));
  EXPECT_CALL(*data_source, ReadFile("/proc/interrupts"))
      .WillOnce(Return(std::string(kX86ProcInterrupts)));

  data_source->Start();

  auto packets = writer_raw_->GetAllTracePackets();
  const protos::gen::InterruptInfo* irq_info = nullptr;
  for (const auto& p : packets) {
    if (p.has_interrupt_info()) {
      irq_info = &p.interrupt_info();
      break;
    }
  }
  ASSERT_NE(irq_info, nullptr);
  // 4 numeric IRQs; NMI and LOC are non-numeric and must be skipped.
  ASSERT_EQ(irq_info->irq_mapping_size(), 4);

  // irq 0: IO-APIC with embedded -edge trigger
  EXPECT_EQ(irq_info->irq_mapping()[0].irq_id(), 0u);
  EXPECT_EQ(irq_info->irq_mapping()[0].name(), "timer");
  // irq 7: IO-APIC with embedded -fasteoi trigger
  EXPECT_EQ(irq_info->irq_mapping()[1].irq_id(), 7u);
  EXPECT_EQ(irq_info->irq_mapping()[1].name(), "pinctrl_amd");
  // irq 31: IR-PCI-MSI -edge with multi-word name
  EXPECT_EQ(irq_info->irq_mapping()[2].irq_id(), 31u);
  EXPECT_EQ(irq_info->irq_mapping()[2].name(), "PCIe PME, PCIe bwctrl");
  // irq 200: no recognised trigger → last-token fallback
  EXPECT_EQ(irq_info->irq_mapping()[3].irq_id(), 200u);
  EXPECT_EQ(irq_info->irq_mapping()[3].name(), "fallback_device");
}

}  // namespace
}  // namespace perfetto
