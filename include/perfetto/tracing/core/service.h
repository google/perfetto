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

#ifndef INCLUDE_PERFETTO_TRACING_CORE_SERVICE_H_
#define INCLUDE_PERFETTO_TRACING_CORE_SERVICE_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <vector>

#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/shared_memory.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

class Consumer;
class DataSourceDescriptor;
class Producer;
class TraceConfig;

// TODO: for the moment this assumes that all the calls hapen on the same
// thread/sequence. Not sure this will be the case long term in Chrome.

// The public API of the tracing Service business logic.
//
// Exposed to:
// 1. The transport layer (e.g., src/unix_rpc/unix_service_host.cc),
//    which forwards commands received from a remote producer or consumer to
//    the actual service implementation.
// 2. Tests.
//
// Subclassed by:
//   The service business logic in src/core/service_impl.cc.
class Service {
 public:
  // The API for the Producer port of the Service.
  // Subclassed by:
  // 1. The service_impl.cc business logic when returning it in response to
  //    the ConnectProducer() method.
  // 2. The transport layer (e.g., src/ipc) when the producer and
  //    the service don't talk locally but via some IPC mechanism.
  class ProducerEndpoint {
   public:
    virtual ~ProducerEndpoint() = default;

    // Called by the Producer to (un)register data sources. The Services returns
    // asynchronousy the ID for the data source.
    // TODO(primiano): thinking twice there is no reason why the service choses
    // ID rather than the Producer. Update in upcoming CLs.
    using RegisterDataSourceCallback = std::function<void(DataSourceID)>;
    virtual void RegisterDataSource(const DataSourceDescriptor&,
                                    RegisterDataSourceCallback) = 0;
    virtual void UnregisterDataSource(DataSourceID) = 0;

    // Called by the Producer to signal that some pages in the shared memory
    // buffer (shared between Service and Producer) have changed.
    virtual void NotifySharedMemoryUpdate(
        const std::vector<uint32_t>& changed_pages) = 0;

    // Returns the SharedMemory buffer for this Producer.
    virtual SharedMemory* shared_memory() const = 0;
  };  // class ProducerEndpoint.

  // The API for the Consumer port of the Service.
  // Subclassed by:
  // 1. The service_impl.cc business logic when returning it in response to
  //    the ConnectConsumer() method.
  // 2. The transport layer (e.g., src/ipc) when the consumer and
  //    the service don't talk locally but via some IPC mechanism.
  class ConsumerEndpoint {
   public:
    virtual ~ConsumerEndpoint() = default;

    virtual void EnableTracing(const TraceConfig&) = 0;
    virtual void DisableTracing() = 0;

    // Tracing data will be delivered invoking Consumer::OnTraceData().
    virtual void ReadBuffers() = 0;

    virtual void FreeBuffers() = 0;
  };  // class ConsumerEndpoint.

  // Implemented in src/core/service_impl.cc .
  static std::unique_ptr<Service> CreateInstance(
      std::unique_ptr<SharedMemory::Factory>,
      base::TaskRunner*);

  virtual ~Service() = default;

  // Connects a Producer instance and obtains a ProducerEndpoint, which is
  // essentially a 1:1 channel between one Producer and the Service.
  // The caller has to guarantee that the passed Producer will be alive as long
  // as the returned ProducerEndpoint is alive.
  // To disconnect just destroy the returned ProducerEndpoint object. It is safe
  // to destroy the Producer once the Producer::OnDisconnect() has been invoked.
  // |shared_buffer_size_hint_bytes| is an optional hint on the size of the
  // shared memory buffer. The service can ignore the hint (e.g., if the hint
  // is unreasonably large).
  virtual std::unique_ptr<ProducerEndpoint> ConnectProducer(
      Producer*,
      size_t shared_buffer_size_hint_bytes = 0) = 0;

  // Coonects a Consumer instance and obtains a ConsumerEndpoint, which is
  // essentially a 1:1 channel between one Consumer and the Service.
  // The caller has to guarantee that the passed Consumer will be alive as long
  // as the returned ConsumerEndpoint is alive.
  // To disconnect just destroy the returned ConsumerEndpoint object. It is safe
  // to destroy the Consumer once the Consumer::OnDisconnect() has been invoked.
  virtual std::unique_ptr<ConsumerEndpoint> ConnectConsumer(Consumer*) = 0;

 public:  // Testing-only
  class ObserverForTesting {
   public:
    virtual ~ObserverForTesting() {}
    virtual void OnProducerConnected(ProducerID) {}
    virtual void OnProducerDisconnected(ProducerID) {}
    virtual void OnDataSourceRegistered(ProducerID, DataSourceID) {}
    virtual void OnDataSourceUnregistered(ProducerID, DataSourceID) {}
    virtual void OnDataSourceInstanceCreated(ProducerID,
                                             DataSourceID,
                                             DataSourceInstanceID) {}
    virtual void OnDataSourceInstanceDestroyed(ProducerID,
                                               DataSourceID,
                                               DataSourceInstanceID) {}
  };

  virtual void set_observer_for_testing(ObserverForTesting*) = 0;
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_CORE_SERVICE_H_
