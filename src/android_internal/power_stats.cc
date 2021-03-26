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

#include "src/android_internal/power_stats.h"

#include "perfetto/ext/base/utils.h"

#include <string.h>

#include <algorithm>
#include <memory>
#include <vector>

// Legacy HAL interfacte for devices shipped before Android S.
#include <android/hardware/power/stats/1.0/IPowerStats.h>

// AIDL interface for Android S+.
#include <android/hardware/power/stats/IPowerStats.h>

#include <binder/IServiceManager.h>

namespace perfetto {
namespace android_internal {

namespace hal = android::hardware::power::stats::V1_0;
namespace aidl = android::hardware::power::stats;

namespace {

// Common interface for data from power stats service.  Devices prior to
// Android S, uses the HAL interface while device from Android S or later
// uses the AIDL interfact.
class PowerStatsDataProvider {
 public:
  virtual bool GetAvailableRails(RailDescriptor*, size_t* size_of_arr) = 0;
  virtual bool GetRailEnergyData(RailEnergyData*, size_t* size_of_arr) = 0;

  // Available from Android S+.
  virtual bool GetEnergyConsumerInfo(EnergyConsumerInfo* consumers,
                                     size_t* size_of_arr) = 0;
  virtual bool GetEnergyConsumed(EnergyEstimationBreakdown* breakdown,
                                 size_t* size_of_arr) = 0;
  virtual ~PowerStatsDataProvider() = default;
};

class PowerStatsHalDataProvider : public PowerStatsDataProvider {
 public:
  bool GetAvailableRails(RailDescriptor*, size_t* size_of_arr) override;
  bool GetRailEnergyData(RailEnergyData*, size_t* size_of_arr) override;
  bool GetEnergyConsumerInfo(EnergyConsumerInfo* consumers,
                             size_t* size_of_arr) override;
  bool GetEnergyConsumed(EnergyEstimationBreakdown* breakdown,
                         size_t* size_of_arr) override;

  PowerStatsHalDataProvider() = default;
  ~PowerStatsHalDataProvider() override = default;

 private:
  android::sp<hal::IPowerStats> svc_;
  hal::IPowerStats* MaybeGetService();
};

class PowerStatsAidlDataProvider : public PowerStatsDataProvider {
 public:
  static constexpr char INSTANCE[] =
      "android.hardware.power.stats.IPowerStats/default";

  bool GetAvailableRails(RailDescriptor*, size_t* size_of_arr) override;
  bool GetRailEnergyData(RailEnergyData*, size_t* size_of_arr) override;
  bool GetEnergyConsumerInfo(EnergyConsumerInfo* consumers,
                             size_t* size_of_arr) override;
  bool GetEnergyConsumed(EnergyEstimationBreakdown* breakdown,
                         size_t* size_of_arr) override;

  PowerStatsAidlDataProvider() = default;
  ~PowerStatsAidlDataProvider() override = default;

 private:
  android::sp<aidl::IPowerStats> svc_;

  aidl::IPowerStats* MaybeGetService();
  void ResetService();
};

PowerStatsDataProvider* GetDataProvider() {
  static std::unique_ptr<PowerStatsDataProvider> data_provider;
  if (data_provider == nullptr) {
    const android::sp<android::IServiceManager> sm =
        android::defaultServiceManager();
    if (sm->isDeclared(
            android::String16(PowerStatsAidlDataProvider::INSTANCE))) {
      data_provider = std::make_unique<PowerStatsAidlDataProvider>();
    } else {
      data_provider = std::make_unique<PowerStatsHalDataProvider>();
    }
  }
  return data_provider.get();
}

}  // anonymous namespace

bool GetAvailableRails(RailDescriptor* descriptor, size_t* size_of_arr) {
  return GetDataProvider()->GetAvailableRails(descriptor, size_of_arr);
}

bool GetRailEnergyData(RailEnergyData* data, size_t* size_of_arr) {
  return GetDataProvider()->GetRailEnergyData(data, size_of_arr);
}

bool GetEnergyConsumerInfo(EnergyConsumerInfo* consumers, size_t* size_of_arr) {
  return GetDataProvider()->GetEnergyConsumerInfo(consumers, size_of_arr);
}

bool GetEnergyConsumed(EnergyEstimationBreakdown* breakdown,
                       size_t* size_of_arr) {
  return GetDataProvider()->GetEnergyConsumed(breakdown, size_of_arr);
}

/*** Power Stats HAL Implemenation *******************************************/

using android::hardware::hidl_vec;
using android::hardware::Return;

hal::IPowerStats* PowerStatsHalDataProvider::MaybeGetService() {
  if (svc_ == nullptr) {
    svc_ = hal::IPowerStats::tryGetService();
  }
  return svc_.get();
}

bool PowerStatsHalDataProvider::GetAvailableRails(
    RailDescriptor* rail_descriptors,
    size_t* size_of_arr) {
  const size_t in_array_size = *size_of_arr;
  *size_of_arr = 0;
  hal::IPowerStats* svc = MaybeGetService();
  if (svc == nullptr) {
    return false;
  }

  hal::Status status;
  auto rails_cb = [rail_descriptors, size_of_arr, &in_array_size, &status](
                      hidl_vec<hal::RailInfo> r, hal::Status s) {
    status = s;
    if (status == hal::Status::SUCCESS) {
      *size_of_arr = std::min(in_array_size, r.size());
      for (int i = 0; i < *size_of_arr; ++i) {
        const hal::RailInfo& rail_info = r[i];
        RailDescriptor& descriptor = rail_descriptors[i];

        descriptor.index = rail_info.index;
        descriptor.sampling_rate = rail_info.samplingRate;

        strncpy(descriptor.rail_name, rail_info.railName.c_str(),
                sizeof(descriptor.rail_name));
        strncpy(descriptor.subsys_name, rail_info.subsysName.c_str(),
                sizeof(descriptor.subsys_name));
        descriptor.rail_name[sizeof(descriptor.rail_name) - 1] = '\0';
        descriptor.subsys_name[sizeof(descriptor.subsys_name) - 1] = '\0';
      }
    }
  };

  Return<void> ret = svc->getRailInfo(rails_cb);
  return status == hal::Status::SUCCESS;
}

bool PowerStatsHalDataProvider::GetRailEnergyData(
    RailEnergyData* rail_energy_array,
    size_t* size_of_arr) {
  const size_t in_array_size = *size_of_arr;
  *size_of_arr = 0;

  hal::IPowerStats* svc = MaybeGetService();
  if (svc == nullptr) {
    return false;
  }

  hal::Status status;
  auto energy_cb = [rail_energy_array, size_of_arr, &in_array_size, &status](
                       hidl_vec<hal::EnergyData> m, hal::Status s) {
    status = s;
    if (status == hal::Status::SUCCESS) {
      *size_of_arr = std::min(in_array_size, m.size());
      for (int i = 0; i < *size_of_arr; ++i) {
        const hal::EnergyData& measurement = m[i];
        RailEnergyData& element = rail_energy_array[i];

        element.index = measurement.index;
        element.timestamp = measurement.timestamp;
        element.energy = measurement.energy;
      }
    }
  };

  Return<void> ret = svc_->getEnergyData(hidl_vec<uint32_t>(), energy_cb);
  return status == hal::Status::SUCCESS;
}

bool PowerStatsHalDataProvider::GetEnergyConsumerInfo(EnergyConsumerInfo*,
                                                      size_t*) {
  return false;
}

bool PowerStatsHalDataProvider::GetEnergyConsumed(EnergyEstimationBreakdown*,
                                                  size_t*) {
  return false;
}

/*** End of Power Stats HAL Implemenation *************************************/

/*** Power Stats AIDL Implemenation *******************************************/
aidl::IPowerStats* PowerStatsAidlDataProvider::MaybeGetService() {
  if (svc_ == nullptr) {
    svc_ = android::checkDeclaredService<aidl::IPowerStats>(
        android::String16(INSTANCE));
  }
  return svc_.get();
}

void PowerStatsAidlDataProvider::ResetService() {
  svc_.clear();
}

bool PowerStatsAidlDataProvider::GetAvailableRails(RailDescriptor* descriptor,
                                                   size_t* size_of_arr) {
  const size_t in_array_size = *size_of_arr;
  *size_of_arr = 0;

  aidl::IPowerStats* svc = MaybeGetService();
  if (svc_ == nullptr) {
    return false;
  }

  std::vector<aidl::Channel> results;
  android::binder::Status status = svc->getEnergyMeterInfo(&results);
  if (!status.isOk()) {
    if (status.transactionError() == android::DEAD_OBJECT) {
      // Service has died.  Reset it to attempt to acquire a new one next time.
      ResetService();
    }
    return false;
  }

  size_t max_size = std::min(in_array_size, results.size());
  for (const auto& result : results) {
    if (*size_of_arr >= max_size) {
      break;
    }
    auto& cur = descriptor[(*size_of_arr)++];
    cur.index = result.id;
    cur.sampling_rate = 0;
    strncpy(cur.rail_name, result.name.c_str(), sizeof(cur.rail_name));
    strncpy(cur.subsys_name, result.subsystem.c_str(), sizeof(cur.subsys_name));
    cur.rail_name[sizeof(cur.rail_name) - 1] = '\0';
    cur.subsys_name[sizeof(cur.subsys_name) - 1] = '\0';
  }
  return true;
}

bool PowerStatsAidlDataProvider::GetRailEnergyData(RailEnergyData* data,
                                                   size_t* size_of_arr) {
  const size_t in_array_size = *size_of_arr;
  *size_of_arr = 0;

  aidl::IPowerStats* svc = MaybeGetService();
  if (svc == nullptr) {
    return false;
  }

  std::vector<int> ids;
  std::vector<aidl::EnergyMeasurement> results;
  android::binder::Status status = svc->readEnergyMeter(ids, &results);
  if (!status.isOk()) {
    if (status.transactionError() == android::DEAD_OBJECT) {
      // Service has died.  Reset it to attempt to acquire a new one next time.
      ResetService();
    }
    return false;
  }

  size_t max_size = std::min(in_array_size, results.size());
  for (const auto& result : results) {
    if (*size_of_arr >= max_size) {
      break;
    }
    auto& cur = data[(*size_of_arr)++];
    cur.index = result.id;
    cur.timestamp = result.timestampMs;
    cur.energy = result.energyUWs;
  }
  return true;
}

bool PowerStatsAidlDataProvider::GetEnergyConsumerInfo(
    EnergyConsumerInfo* consumers,
    size_t* size_of_arr) {
  const size_t in_array_size = *size_of_arr;
  *size_of_arr = 0;

  aidl::IPowerStats* svc = MaybeGetService();
  if (svc == nullptr) {
    return false;
  }
  std::vector<aidl::EnergyConsumer> results;
  android::binder::Status status = svc->getEnergyConsumerInfo(&results);

  if (!status.isOk()) {
    if (status.transactionError() == android::DEAD_OBJECT) {
      // Service has died.  Reset it to attempt to acquire a new one next time.
      ResetService();
    }
    return false;
  }
  size_t max_size = std::min(in_array_size, results.size());
  for (const auto& result : results) {
    if (*size_of_arr >= max_size) {
      break;
    }
    auto& cur = consumers[(*size_of_arr)++];
    cur.energy_consumer_id = result.id;
    cur.ordinal = result.ordinal;
    strncpy(cur.type, aidl::toString(result.type).c_str(), sizeof(cur.type));
    cur.type[sizeof(cur.type) - 1] = '\0';
    strncpy(cur.name, result.name.c_str(), sizeof(cur.name));
    cur.name[sizeof(cur.name) - 1] = '\0';
  }
  return true;
}
bool PowerStatsAidlDataProvider::GetEnergyConsumed(
    EnergyEstimationBreakdown* breakdown,
    size_t* size_of_arr) {
  const size_t in_array_size = *size_of_arr;
  *size_of_arr = 0;

  aidl::IPowerStats* svc = MaybeGetService();
  if (svc == nullptr) {
    return false;
  }

  std::vector<int> ids;
  std::vector<aidl::EnergyConsumerResult> results;
  android::binder::Status status = svc->getEnergyConsumed(ids, &results);

  if (!status.isOk()) {
    if (status.transactionError() == android::DEAD_OBJECT) {
      // Service has died.  Reset it to attempt to acquire a new one next time.
      ResetService();
    }
    return false;
  }

  size_t max_size = std::min(in_array_size, results.size());
  // Iterate through all consumer ID.
  for (const auto& result : results) {
    if (*size_of_arr >= max_size) {
      break;
    }
    auto& cur = breakdown[(*size_of_arr)++];
    cur.energy_consumer_id = result.id;
    cur.uid = ALL_UIDS_FOR_CONSUMER;
    cur.energy_uws = result.energyUWs;

    // Iterate through all UIDs for this consumer.
    for (const auto& attribution : result.attribution) {
      if (*size_of_arr >= max_size) {
        break;
      }
      auto& cur = breakdown[(*size_of_arr)++];
      cur.energy_consumer_id = result.id;
      cur.uid = attribution.uid;
      cur.energy_uws = attribution.energyUWs;
    }
  }
  return true;
}
/*** End of Power Stats AIDL Implemenation ************************************/

}  // namespace android_internal
}  // namespace perfetto
