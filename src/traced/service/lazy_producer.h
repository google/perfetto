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

#ifndef SRC_TRACED_SERVICE_LAZY_PRODUCER_H_
#define SRC_TRACED_SERVICE_LAZY_PRODUCER_H_

#include <set>
#include <string>

#include "perfetto/base/task_runner.h"
#include "perfetto/base/weak_ptr.h"

#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/tracing_service.h"

namespace perfetto {

class LazyProducer : public Producer {
 public:
  LazyProducer(base::TaskRunner* task_runner,
               uint32_t delay_ms,
               std::string data_source_name,
               std::string property_name);

  ~LazyProducer() override;
  // No-ops to satisfy the Producer implementation.
  void OnDisconnect() override {}
  void OnTracingSetup() override {}
  void StartDataSource(DataSourceInstanceID, const DataSourceConfig&) override {
  }
  void Flush(FlushRequestID flush_id,
             const DataSourceInstanceID*,
             size_t) override {
    endpoint_->NotifyFlushComplete(flush_id);
  }

  void OnConnect() override;
  void SetupDataSource(DataSourceInstanceID, const DataSourceConfig&) override;
  void StopDataSource(DataSourceInstanceID) override;

  void ConnectInProcess(TracingService* svc);
  virtual bool SetAndroidProperty(const std::string& name,
                                  const std::string& value);

 private:
  base::TaskRunner* task_runner_;
  uint32_t delay_ms_;

  std::string data_source_name_;
  std::string property_name_;

  std::unique_ptr<TracingService::ProducerEndpoint> endpoint_;
  uint64_t active_sessions_ = 0;
  uint64_t generation_ = 0;

  base::WeakPtrFactory<LazyProducer> weak_factory_;  // Keep last.
};

}  // namespace perfetto

#endif  // SRC_TRACED_SERVICE_LAZY_PRODUCER_H_
