/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/traced/probes/power/linux_power_sysfs_data_source.h"
#include "src/base/test/tmp_dir_tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

TEST(LinuxPowerSysfsDataSourceTest, BatteryCounters) {
  base::TmpDirTree tmpdir;
  std::unique_ptr<LinuxPowerSysfsDataSource::BatteryInfo> battery_info_;

  tmpdir.AddDir("BAT0");
  tmpdir.AddFile("BAT0/type", "Battery\n");
  tmpdir.AddFile("BAT0/present", "1\n");
  tmpdir.AddFile("BAT0/capacity", "95\n");         // 95 percent.
  tmpdir.AddFile("BAT0/charge_now", "3074000\n");  // 3074000 µAh.
  tmpdir.AddFile("BAT0/current_now", "245000\n");  // 245000 µA.
  tmpdir.AddFile("BAT0/current_avg", "240000\n");  // 240000 µA.

  battery_info_.reset(
      new LinuxPowerSysfsDataSource::BatteryInfo(tmpdir.path().c_str()));

  EXPECT_EQ(battery_info_->num_batteries(), 1u);
  EXPECT_EQ(*battery_info_->GetCapacityPercent(0), 95);
  EXPECT_EQ(*battery_info_->GetCurrentNowUa(0), 245000);
  EXPECT_EQ(*battery_info_->GetAverageCurrentUa(0), 240000);
  EXPECT_EQ(*battery_info_->GetChargeCounterUah(0), 3074000);
}

TEST(LinuxPowerSysfsDataSourceTest, HidDeviceCounters) {
  base::TmpDirTree tmpdir;
  std::unique_ptr<LinuxPowerSysfsDataSource::BatteryInfo> battery_info_;

  // Some HID devices (e.g. stylus) can also report battery info.
  tmpdir.AddDir("hid-0001-battery");
  tmpdir.AddFile("hid-0001-battery/type", "Battery\n");
  tmpdir.AddFile("hid-0001-battery/present", "1\n");
  tmpdir.AddFile("hid-0001-battery/capacity", "88\n");  // 88 percent.
  // The HID device only reports the battery capacity in percent.

  battery_info_.reset(
      new LinuxPowerSysfsDataSource::BatteryInfo(tmpdir.path().c_str()));

  EXPECT_EQ(battery_info_->num_batteries(), 1u);
  EXPECT_EQ(*battery_info_->GetCapacityPercent(0), 88);
  EXPECT_EQ(battery_info_->GetCurrentNowUa(0), base::nullopt);
  EXPECT_EQ(battery_info_->GetAverageCurrentUa(0), base::nullopt);
  EXPECT_EQ(battery_info_->GetChargeCounterUah(0), base::nullopt);
}

TEST(LinuxPowerSysfsDataSourceTest, MultipleBatteries) {
  base::TmpDirTree tmpdir;
  std::unique_ptr<LinuxPowerSysfsDataSource::BatteryInfo> battery_info_;

  // Some HID devices (e.g. stylus) can also report battery info.
  tmpdir.AddDir("hid-0001-battery");
  tmpdir.AddFile("hid-0001-battery/type", "Battery\n");
  tmpdir.AddFile("hid-0001-battery/present", "1\n");
  tmpdir.AddFile("hid-0001-battery/capacity", "88\n");  // 88 percent.
  // The HID device only reports the battery capacity in percent.

  // Add the main battery.
  tmpdir.AddDir("BAT0");
  tmpdir.AddFile("BAT0/type", "Battery\n");
  tmpdir.AddFile("BAT0/present", "1\n");
  tmpdir.AddFile("BAT0/capacity", "95\n");         // 95 percent.
  tmpdir.AddFile("BAT0/charge_now", "3074000\n");  // 3074000 µAh.
  tmpdir.AddFile("BAT0/current_now", "245000\n");  // 245000 µA.
  tmpdir.AddFile("BAT0/current_avg", "240000\n");  // 240000 µA.

  battery_info_.reset(
      new LinuxPowerSysfsDataSource::BatteryInfo(tmpdir.path().c_str()));

  EXPECT_EQ(battery_info_->num_batteries(), 2u);
  size_t main_battery_idx = battery_info_->GetBatteryName(0) == "BAT0" ? 0 : 1;
  size_t second_battery_idx = main_battery_idx == 0 ? 1 : 0;

  EXPECT_EQ(*battery_info_->GetCapacityPercent(second_battery_idx), 88);
  EXPECT_EQ(battery_info_->GetCurrentNowUa(second_battery_idx), base::nullopt);
  EXPECT_EQ(battery_info_->GetAverageCurrentUa(second_battery_idx),
            base::nullopt);
  EXPECT_EQ(battery_info_->GetChargeCounterUah(second_battery_idx),
            base::nullopt);

  EXPECT_EQ(*battery_info_->GetCapacityPercent(main_battery_idx), 95);
  EXPECT_EQ(*battery_info_->GetCurrentNowUa(main_battery_idx), 245000);
  EXPECT_EQ(*battery_info_->GetAverageCurrentUa(main_battery_idx), 240000);
  EXPECT_EQ(*battery_info_->GetChargeCounterUah(main_battery_idx), 3074000);
}

TEST(LinuxPowerSysfsDataSourceTest, EnergyNow) {
  base::TmpDirTree tmpdir;
  std::unique_ptr<LinuxPowerSysfsDataSource::BatteryInfo> battery_info_;

  tmpdir.AddDir("BAT0");
  tmpdir.AddFile("BAT0/type", "Battery\n");
  tmpdir.AddFile("BAT0/present", "1\n");
  tmpdir.AddFile("BAT0/capacity", "95\n");          // 95 percent.
  tmpdir.AddFile("BAT0/energy_now", "56680000\n");  // 56680000 µWh.

  battery_info_.reset(
      new LinuxPowerSysfsDataSource::BatteryInfo(tmpdir.path().c_str()));

  EXPECT_EQ(battery_info_->num_batteries(), 1u);
  EXPECT_EQ(*battery_info_->GetCapacityPercent(0), 95);
  EXPECT_EQ(*battery_info_->GetEnergyCounterUah(0), 56680000);
}

TEST(LinuxPowerSysfsDataSourceTest, EnergyVoltageNow) {
  base::TmpDirTree tmpdir;
  std::unique_ptr<LinuxPowerSysfsDataSource::BatteryInfo> battery_info_;

  tmpdir.AddDir("BAT0");
  tmpdir.AddFile("BAT0/type", "Battery\n");
  tmpdir.AddFile("BAT0/present", "1\n");
  tmpdir.AddFile("BAT0/capacity", "95\n");           // 95 percent.
  tmpdir.AddFile("BAT0/voltage_now", "17356000\n");  // Now at 17.356 µV.

  battery_info_.reset(
      new LinuxPowerSysfsDataSource::BatteryInfo(tmpdir.path().c_str()));

  EXPECT_EQ(battery_info_->num_batteries(), 1u);
  EXPECT_EQ(*battery_info_->GetCapacityPercent(0), 95);
  EXPECT_EQ(*battery_info_->GetVoltageUv(0), 17356000);
}

}  // namespace
}  // namespace perfetto
