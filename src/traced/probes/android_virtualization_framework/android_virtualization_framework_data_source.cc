/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/traced/probes/android_virtualization_framework/android_virtualization_framework_data_source.h"

#include "android_virtualization_framework_data_source.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"

#include "protos/perfetto/config/android/android_virtualization_framework_config.pbzero.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/system_properties.h>
#endif

namespace perfetto {

namespace {

static bool SetAndroidSysProp(const std::string& name,
                              const std::string& value) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  return __system_property_set(name.c_str(), value.c_str()) == 0;
#else
  base::ignore_result(name);
  base::ignore_result(value);
  return true;
#endif
}

}  // namespace

// static
const ProbesDataSource::Descriptor
    AndroidVirtualizationFrameworkDataSource::descriptor = {
        /*name*/ "android.virtualization_framework",
        /*flags*/ Descriptor::kFlagsNone,
        /*fill_descriptor_func*/ nullptr,
};

AndroidVirtualizationFrameworkDataSource::
    AndroidVirtualizationFrameworkDataSource(const DataSourceConfig& cfg,
                                             TracingSessionID session_id)
    : ProbesDataSource(session_id, &descriptor) {
  using protos::pbzero::AndroidVirtualizationFrameworkConfig;
  AndroidVirtualizationFrameworkConfig::Decoder config(
      cfg.android_virtualization_framework_config_raw());
  for (auto it = config.android_vm_config(); it; ++it) {
    AndroidVirtualizationFrameworkConfig::AndroidVmConfig::Decoder vm_config(
        it->as_bytes());
    vm_configs_.emplace_back(vm_config.name().ToStdString());
  }
}

AndroidVirtualizationFrameworkDataSource::
    ~AndroidVirtualizationFrameworkDataSource() {
  for (const auto& vm_config : vm_configs_) {
    std::string prop_name =
        "persist.avf_vm.traced_relay.enable." + vm_config.name_;
    if (!SetAndroidSysProp(prop_name, "0")) {
      PERFETTO_ELOG("Failed to stop traced_relay for VM %s",
                    vm_config.name_.c_str());
    }
  }
}

void AndroidVirtualizationFrameworkDataSource::Start() {
  for (const auto& vm_config : vm_configs_) {
    std::string prop_name =
        "persist.avf_vm.traced_relay.enable." + vm_config.name_;
    if (!SetAndroidSysProp(prop_name, "1")) {
      PERFETTO_ELOG("Failed to start traced_relay for VM %s",
                    vm_config.name_.c_str());
    }
  }
}

void AndroidVirtualizationFrameworkDataSource::Flush(FlushRequestID,
                                                     std::function<void()>) {
  // No-op
}

}  // namespace perfetto
