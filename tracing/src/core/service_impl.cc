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

#include "tracing/src/core/service_impl.h"

#include <inttypes.h>

#include "base/logging.h"
#include "base/task_runner.h"
#include "tracing/core/data_source_config.h"
#include "tracing/core/producer.h"
#include "tracing/core/shared_memory.h"

namespace perfetto {

// TODO add ThreadChecker everywhere.

namespace {
constexpr size_t kShmSize = 4096;  // TODO: temporary.
}  // namespace

// static
std::unique_ptr<Service> Service::CreateInstance(
    std::unique_ptr<SharedMemory::Factory> shm_factory,
    base::TaskRunner* task_runner) {
  return std::unique_ptr<Service>(
      new ServiceImpl(std::move(shm_factory), task_runner));
}

ServiceImpl::ServiceImpl(std::unique_ptr<SharedMemory::Factory> shm_factory,
                         base::TaskRunner* task_runner)
    : shm_factory_(std::move(shm_factory)), task_runner_(task_runner) {
  PERFETTO_DCHECK(task_runner_);
}

ServiceImpl::~ServiceImpl() {
  // TODO handle teardown of all Producer.
}

std::unique_ptr<Service::ProducerEndpoint> ServiceImpl::ConnectProducer(
    Producer* producer) {
  const ProducerID id = ++last_producer_id_;
  auto shared_memory = shm_factory_->CreateSharedMemory(kShmSize);
  std::unique_ptr<ProducerEndpointImpl> endpoint(new ProducerEndpointImpl(
      id, this, task_runner_, producer, std::move(shared_memory)));
  auto it_and_inserted = producers_.emplace(id, endpoint.get());
  PERFETTO_DCHECK(it_and_inserted.second);
  task_runner_->PostTask(std::bind(&Producer::OnConnect, endpoint->producer(),
                                   id, endpoint->shared_memory()));
  return std::move(endpoint);
}

void ServiceImpl::DisconnectProducer(ProducerID id) {
  PERFETTO_DCHECK(producers_.count(id));
  producers_.erase(id);
}

Service::ProducerEndpoint* ServiceImpl::GetProducer(ProducerID id) const {
  auto it = producers_.find(id);
  if (it == producers_.end())
    return nullptr;
  return it->second;
}

////////////////////////////////////////////////////////////////////////////////
// ServiceImpl::ProducerEndpointImpl implementation
////////////////////////////////////////////////////////////////////////////////

ServiceImpl::ProducerEndpointImpl::ProducerEndpointImpl(
    ProducerID id,
    ServiceImpl* service,
    base::TaskRunner* task_runner,
    Producer* producer,
    std::unique_ptr<SharedMemory> shared_memory)
    : id_(id),
      service_(service),
      task_runner_(task_runner),
      producer_(std::move(producer)),
      shared_memory_(std::move(shared_memory)) {}

ServiceImpl::ProducerEndpointImpl::~ProducerEndpointImpl() {
  task_runner_->PostTask(std::bind(&Producer::OnDisconnect, producer_));
  service_->DisconnectProducer(id_);
}

ProducerID ServiceImpl::ProducerEndpointImpl::GetID() const {
  return id_;
}

void ServiceImpl::ProducerEndpointImpl::RegisterDataSource(
    const DataSourceDescriptor&,
    RegisterDataSourceCallback callback) {
  const DataSourceID dsid = ++last_data_source_id_;
  PERFETTO_DLOG("[ServiceImpl] RegisterDataSource from producer %" PRIu64, id_);
  task_runner_->PostTask(std::bind(std::move(callback), dsid));
  // TODO implement the bookkeeping logic.
}

void ServiceImpl::ProducerEndpointImpl::UnregisterDataSource(
    DataSourceID dsid) {
  PERFETTO_DLOG("[ServiceImpl] UnregisterDataSource(%" PRIu64
                ") from producer %" PRIu64,
                dsid, id_);
  PERFETTO_CHECK(dsid);
  // TODO implement the bookkeeping logic.
  return;
}

void ServiceImpl::ProducerEndpointImpl::NotifyPageAcquired(uint32_t page) {
  PERFETTO_DLOG("[ServiceImpl] NotifyPageAcquired(%" PRIu32
                ") from producer %" PRIu64,
                page, id_);
  // TODO implement the bookkeeping logic.
  return;
}

void ServiceImpl::ProducerEndpointImpl::NotifyPageReleased(uint32_t page) {
  PERFETTO_DLOG("[ServiceImpl] NotifyPageReleased(%" PRIu32
                ") from producer %" PRIu64,
                page, id_);
  PERFETTO_DCHECK(shared_memory_);
  PERFETTO_DLOG("[ServiceImpl] Reading Shared memory: \"%s\"",
                reinterpret_cast<const char*>(shared_memory_->start()));
  // TODO implement the bookkeeping logic.
  return;
}

}  // namespace perfetto
