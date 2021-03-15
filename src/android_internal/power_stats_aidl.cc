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

#include "src/android_internal/power_stats_aidl.h"

#include "perfetto/ext/base/utils.h"

#include <vector>

#include <android/hardware/power/stats/IPowerStats.h>
#include <binder/IServiceManager.h>

namespace perfetto {
namespace android_internal {

using android::hardware::power::stats::EnergyConsumerResult;
using android::hardware::power::stats::IPowerStats;

namespace {

android::sp<IPowerStats> g_svc;

IPowerStats* MaybeGetService() {
  if (!g_svc) {
    static const char kInstance[] =
        "android.hardware.power.stats.IPowerStats/default";
    g_svc = android::checkDeclaredService<IPowerStats>(
        android::String16(kInstance));
  }
  return g_svc.get();
}

void ResetService() {
  g_svc.clear();
}

}  // namespace

bool GetEnergyConsumed(EnergyEstimationBreakdown* breakdown,
                       size_t* size_of_arr) {
  const size_t in_array_size = *size_of_arr;
  *size_of_arr = 0;
  IPowerStats* svc = MaybeGetService();
  if (svc == nullptr) {
    return false;
  }

  std::vector<int> ids;
  std::vector<EnergyConsumerResult> results;
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

}  // namespace android_internal
}  // namespace perfetto
