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

#ifndef TRACING_INCLUDE_TRACING_CORE_SERVICE_H_
#define TRACING_INCLUDE_TRACING_CORE_SERVICE_H_

#include <stdint.h>

#include <functional>
#include <memory>
#include <vector>

#include "tracing/core/basic_types.h"
#include "tracing/core/shared_memory.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

class DataSourceConfig;
class DataSourceDescriptor;
class Producer;

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
  };  // class ProducerEndpoint.

  // Implemented in src/core/service_impl.cc .
  static std::unique_ptr<Service> CreateInstance(
      std::unique_ptr<SharedMemory::Factory>,
      base::TaskRunner*);

  virtual ~Service() = default;

  // Connects a Producer instance and obtains a ProducerEndpoint, which is
  // essentially a 1:1 channel between one Producer an the Service.
  // The caller has to guarantee that the Producer will be alive as long as
  // the returned ProducerEndpoint is alive.
  // To disconnect just destroy the returned ProducerEndpoint object. It is safe
  // to destroy the Producer once the Producer::OnDisconnect() has been invoked.
  virtual std::unique_ptr<ProducerEndpoint> ConnectProducer(Producer*) = 0;

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

#endif  // TRACING_INCLUDE_TRACING_CORE_SERVICE_H_
