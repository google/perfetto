/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACING_CORE_SERVICE_IMPL_H_
#define SRC_TRACING_CORE_SERVICE_IMPL_H_

#include <functional>
#include <map>
#include <memory>
#include <set>

#include "perfetto/base/weak_ptr.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/service.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

class Consumer;
class DataSourceConfig;
class Producer;
class SharedMemory;
class TraceConfig;

// The tracing service business logic.
class ServiceImpl : public Service {
 public:
  // The implementation behind the service endpoint exposed to each producer.
  class ProducerEndpointImpl : public Service::ProducerEndpoint {
   public:
    ProducerEndpointImpl(ProducerID,
                         ServiceImpl*,
                         base::TaskRunner*,
                         Producer*,
                         std::unique_ptr<SharedMemory>);
    ~ProducerEndpointImpl() override;

    Producer* producer() const { return producer_; }

    // Service::ProducerEndpoint implementation.
    void RegisterDataSource(const DataSourceDescriptor&,
                            RegisterDataSourceCallback) override;
    void UnregisterDataSource(DataSourceID) override;

    void NotifySharedMemoryUpdate(
        const std::vector<uint32_t>& changed_pages) override;

    SharedMemory* shared_memory() const override;

   private:
    ProducerEndpointImpl(const ProducerEndpointImpl&) = delete;
    ProducerEndpointImpl& operator=(const ProducerEndpointImpl&) = delete;

    ProducerID const id_;
    ServiceImpl* const service_;
    base::TaskRunner* const task_runner_;
    Producer* producer_;
    std::unique_ptr<SharedMemory> shared_memory_;
    DataSourceID last_data_source_id_ = 0;
  };

  // The implementation behind the service endpoint exposed to each consumer.
  class ConsumerEndpointImpl : public Service::ConsumerEndpoint {
   public:
    ConsumerEndpointImpl(ServiceImpl*, base::TaskRunner*, Consumer*);
    ~ConsumerEndpointImpl() override;

    Consumer* consumer() const { return consumer_; }
    base::WeakPtr<ConsumerEndpointImpl> GetWeakPtr();

    // Service::ConsumerEndpoint implementation.
    void EnableTracing(const TraceConfig&) override;
    void DisableTracing() override;
    void ReadBuffers() override;
    void FreeBuffers() override;

   private:
    ConsumerEndpointImpl(const ConsumerEndpointImpl&) = delete;
    ConsumerEndpointImpl& operator=(const ConsumerEndpointImpl&) = delete;

    ServiceImpl* const service_;
    Consumer* const consumer_;
    base::WeakPtrFactory<ConsumerEndpointImpl> weak_ptr_factory_;
  };

  explicit ServiceImpl(std::unique_ptr<SharedMemory::Factory>,
                       base::TaskRunner*);
  ~ServiceImpl() override;

  // Called by ProducerEndpointImpl.
  void DisconnectProducer(ProducerID);

  // Called by ConsumerEndpointImpl.
  void DisconnectConsumer(ConsumerEndpointImpl*);
  void EnableTracing(ConsumerEndpointImpl*, const TraceConfig&);
  void DisableTracing(ConsumerEndpointImpl*);
  void ReadBuffers(ConsumerEndpointImpl*);
  void FreeBuffers(ConsumerEndpointImpl*);

  // Service implementation.
  std::unique_ptr<Service::ProducerEndpoint> ConnectProducer(
      Producer*,
      size_t shared_buffer_size_hint_bytes = 0) override;

  std::unique_ptr<Service::ConsumerEndpoint> ConnectConsumer(
      Consumer*) override;

  void set_observer_for_testing(ObserverForTesting*) override;

  // Exposed mainly for testing.
  size_t num_producers() const { return producers_.size(); }
  ProducerEndpointImpl* GetProducer(ProducerID) const;

 private:
  ServiceImpl(const ServiceImpl&) = delete;
  ServiceImpl& operator=(const ServiceImpl&) = delete;

  std::unique_ptr<SharedMemory::Factory> shm_factory_;
  base::TaskRunner* const task_runner_;
  ProducerID last_producer_id_ = 0;
  std::map<ProducerID, ProducerEndpointImpl*> producers_;
  std::set<ConsumerEndpointImpl*> consumers_;
  ObserverForTesting* observer_ = nullptr;
};

}  // namespace perfetto

#endif  // SRC_TRACING_CORE_SERVICE_IMPL_H_
