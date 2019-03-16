/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/traced/service/lazy_producer.h"

#include "perfetto/base/build_config.h"

#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/system_properties.h>
#endif

namespace perfetto {

LazyProducer::LazyProducer(base::TaskRunner* task_runner,
                           uint32_t delay_ms,
                           std::string data_source_name,
                           std::string property_name)
    : task_runner_(task_runner),
      delay_ms_(delay_ms),
      data_source_name_(data_source_name),
      property_name_(property_name),
      weak_factory_(this) {}

void LazyProducer::ConnectInProcess(TracingService* svc) {
  endpoint_ = svc->ConnectProducer(this, geteuid(), "lazy_producer",
                                   /*shm_hint_kb*/ 16);
}

void LazyProducer::OnConnect() {
  DataSourceDescriptor dsd;
  dsd.set_name(data_source_name_);
  endpoint_->RegisterDataSource(dsd);
}

void LazyProducer::SetupDataSource(DataSourceInstanceID,
                                   const DataSourceConfig&) {
  SetAndroidProperty(property_name_, "1");
  active_sessions_++;
  generation_++;
}

void LazyProducer::StopDataSource(DataSourceInstanceID) {
  if (--active_sessions_)
    return;

  uint64_t cur_generation = generation_;
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, cur_generation] {
        if (!weak_this)
          return;
        if (weak_this->generation_ == cur_generation)
          weak_this->SetAndroidProperty(weak_this->property_name_, "0");
      },
      delay_ms_);
}

bool LazyProducer::SetAndroidProperty(const std::string& name,
                                      const std::string& value) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  return __system_property_set(name.c_str(), value.c_str()) == 0;
#else
  // Allow this to be mocked out for tests on other platforms.
  base::ignore_result(name);
  base::ignore_result(value);
  return true;
#endif
}

LazyProducer::~LazyProducer() {
  if (active_sessions_)
    SetAndroidProperty(property_name_, "0");
}

}  // namespace perfetto
