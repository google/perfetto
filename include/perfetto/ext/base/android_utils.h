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

#ifndef INCLUDE_PERFETTO_EXT_BASE_ANDROID_UTILS_H_
#define INCLUDE_PERFETTO_EXT_BASE_ANDROID_UTILS_H_

#include <optional>
#include <string>

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <sys/utsname.h>
#endif

namespace perfetto {
namespace base {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

// Returns the value of the Android system property named `name`. If the
// property does not exist, returns an empty string (a non-existing property is
// the same as a property with an empty value for this API).
std::string GetAndroidProp(const char* name);

#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

struct SystemInfo {
  std::string tracing_service_version;
  std::optional<int32_t> timezone_off_mins;
  std::optional<struct utsname> utsname_info;
  std::optional<uint32_t> page_size;
  std::optional<uint32_t> num_cpus;
  std::string android_build_fingerprint;
  std::string android_device_manufacturer;
  std::optional<uint64_t> android_sdk_version;
  std::string android_soc_model;
  std::string android_guest_soc_model;
  std::string android_hardware_revision;
  std::string android_storage_model;
  std::string android_ram_model;
};

// Returns the device's system information.
SystemInfo GetSystemInfo();

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_ANDROID_UTILS_H_
