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

#include "src/traced/service/builtin_producer.h"

#include <sys/types.h>
#include <unistd.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "src/tracing/core/metatrace_writer.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/system_properties.h>
#endif

namespace perfetto {

namespace {

constexpr char kHeapprofdDataSourceName[] = "android.heapprofd";
constexpr char kLazyHeapprofdPropertyName[] = "traced.lazy.heapprofd";

}  // namespace

BuiltinProducer::BuiltinProducer(base::TaskRunner* task_runner,
                                 uint32_t lazy_stop_delay_ms)
    : task_runner_(task_runner), weak_factory_(this) {
  lazy_heapprofd_.stop_delay_ms = lazy_stop_delay_ms;
}

BuiltinProducer::~BuiltinProducer() {
  if (!lazy_heapprofd_.instance_ids.empty())
    SetAndroidProperty(kLazyHeapprofdPropertyName, "0");
}

void BuiltinProducer::ConnectInProcess(TracingService* svc) {
  endpoint_ = svc->ConnectProducer(this, geteuid(), "traced",
                                   /*shm_hint_kb*/ 16, /*in_process*/ true);
}

void BuiltinProducer::OnConnect() {
  DataSourceDescriptor metatrace_dsd;
  metatrace_dsd.set_name(MetatraceWriter::kDataSourceName);
  metatrace_dsd.set_will_notify_on_stop(true);
  endpoint_->RegisterDataSource(metatrace_dsd);

  DataSourceDescriptor lazy_heapprofd_dsd;
  lazy_heapprofd_dsd.set_name(kHeapprofdDataSourceName);
  endpoint_->RegisterDataSource(lazy_heapprofd_dsd);
}

void BuiltinProducer::SetupDataSource(DataSourceInstanceID ds_id,
                                      const DataSourceConfig& ds_config) {
  if (ds_config.name() == kHeapprofdDataSourceName) {
    SetAndroidProperty(kLazyHeapprofdPropertyName, "1");
    lazy_heapprofd_.generation++;
    lazy_heapprofd_.instance_ids.emplace(ds_id);
  }
}

void BuiltinProducer::StartDataSource(DataSourceInstanceID ds_id,
                                      const DataSourceConfig& ds_config) {
  // We slightly rely on the fact that since this producer is in-process for
  // enabling metatrace early (relative to producers that are notified via IPC).
  if (ds_config.name() == MetatraceWriter::kDataSourceName) {
    auto writer = endpoint_->CreateTraceWriter(
        static_cast<BufferID>(ds_config.target_buffer()));

    auto it_and_inserted = metatrace_.writers.emplace(
        std::piecewise_construct, std::make_tuple(ds_id), std::make_tuple());
    PERFETTO_DCHECK(it_and_inserted.second);
    // Note: only the first concurrent writer will actually be active.
    metatrace_.writers[ds_id].Enable(task_runner_, std::move(writer),
                                     metatrace::TAG_ANY);
  }
}

void BuiltinProducer::StopDataSource(DataSourceInstanceID ds_id) {
  auto meta_it = metatrace_.writers.find(ds_id);
  if (meta_it != metatrace_.writers.end()) {
    // Synchronously re-flush the metatrace writer to record more of the
    // teardown interactions, then ack the stop.
    meta_it->second.WriteAllAndFlushTraceWriter([] {});
    metatrace_.writers.erase(meta_it);
    endpoint_->NotifyDataSourceStopped(ds_id);
  }

  auto lazy_it = lazy_heapprofd_.instance_ids.find(ds_id);
  if (lazy_it != lazy_heapprofd_.instance_ids.end()) {
    lazy_heapprofd_.instance_ids.erase(lazy_it);

    // if no more sessions - stop heapprofd after a delay
    if (lazy_heapprofd_.instance_ids.empty()) {
      uint64_t cur_generation = lazy_heapprofd_.generation;
      auto weak_this = weak_factory_.GetWeakPtr();
      task_runner_->PostDelayedTask(
          [weak_this, cur_generation] {
            if (!weak_this)
              return;
            if (weak_this->lazy_heapprofd_.generation == cur_generation)
              weak_this->SetAndroidProperty(kLazyHeapprofdPropertyName, "0");
          },
          lazy_heapprofd_.stop_delay_ms);
    }
  }
}

void BuiltinProducer::Flush(FlushRequestID flush_id,
                            const DataSourceInstanceID* ds_ids,
                            size_t num_ds_ids) {
  for (size_t i = 0; i < num_ds_ids; i++) {
    auto meta_it = metatrace_.writers.find(ds_ids[i]);
    if (meta_it != metatrace_.writers.end()) {
      meta_it->second.WriteAllAndFlushTraceWriter([] {});
    }
    // nothing to be done for lazy heapprofd sources
  }
  endpoint_->NotifyFlushComplete(flush_id);
}

bool BuiltinProducer::SetAndroidProperty(const std::string& name,
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

}  // namespace perfetto
