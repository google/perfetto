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

#ifndef SRC_ANDROID_INTERNAL_POWER_STATS_AIDL_H_
#define SRC_ANDROID_INTERNAL_POWER_STATS_AIDL_H_

#include <stddef.h>
#include <stdint.h>

namespace perfetto {
namespace android_internal {

const int32_t ALL_UIDS_FOR_CONSUMER = -1;

struct EnergyEstimationBreakdown {
  // Energy consumer ID.
  int32_t energy_consumer_id;

  // Process uid.  ALL_UIDS_FOR_CONSUMER represents energy for all processes
  // for the energy_consumer_id.
  int32_t uid;

  // Energy usage in microwatts-second(µWs).
  int64_t energy_uws;
};

extern "C" {

// These functions are not thread safe unless specified otherwise.

// Retrieve the energy estimation breakdown for all energy consumer.  For each
// consumer, there will be an entry with a uid of ALL_UIDS_FOR_CONSUMER,
// followed by the energy breakdown for each process contributing to that
// consumer.
bool __attribute__((visibility("default")))
GetEnergyConsumed(EnergyEstimationBreakdown* breakdown, size_t* size_of_arr);

}  // extern "C"

}  // namespace android_internal
}  // namespace perfetto

#endif  // SRC_ANDROID_INTERNAL_POWER_STATS_AIDL_H_
