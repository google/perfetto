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

#include "src/tracing/core/service_impl.h"

#include <inttypes.h>

#include <algorithm>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/shared_memory.h"

namespace perfetto {

// TODO add ThreadChecker everywhere.

namespace {
constexpr size_t kPageSize = 4096;
constexpr size_t kDefaultShmSize = kPageSize * 16;  // 64 KB.
constexpr size_t kMaxShmSize = kPageSize * 1024;    // 4 MB.
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
    Producer* producer,
    size_t shared_buffer_size_hint_bytes) {
  const ProducerID id = ++last_producer_id_;

  size_t shm_size = std::min(shared_buffer_size_hint_bytes, kMaxShmSize);
  if (shm_size % kPageSize || shm_size < kPageSize)
    shm_size = kDefaultShmSize;

  // TODO(primiano): right now Create() will suicide in case of OOM if the mmap
  // fails. We should instead gracefully fail the request and tell the client
  // to go away.
  auto shared_memory = shm_factory_->CreateSharedMemory(shm_size);
  std::unique_ptr<ProducerEndpointImpl> endpoint(new ProducerEndpointImpl(
      id, this, task_runner_, producer, std::move(shared_memory)));
  auto it_and_inserted = producers_.emplace(id, endpoint.get());
  PERFETTO_DCHECK(it_and_inserted.second);
  task_runner_->PostTask(std::bind(&Producer::OnConnect, endpoint->producer()));
  if (observer_)
    observer_->OnProducerConnected(id);
  return std::move(endpoint);
}

void ServiceImpl::DisconnectProducer(ProducerID id) {
  PERFETTO_DCHECK(producers_.count(id));
  producers_.erase(id);
  if (observer_)
    observer_->OnProducerDisconnected(id);
}

ServiceImpl::ProducerEndpointImpl* ServiceImpl::GetProducer(
    ProducerID id) const {
  auto it = producers_.find(id);
  if (it == producers_.end())
    return nullptr;
  return it->second;
}

std::unique_ptr<Service::ConsumerEndpoint> ServiceImpl::ConnectConsumer(
    Consumer* consumer) {
  std::unique_ptr<ConsumerEndpointImpl> endpoint(
      new ConsumerEndpointImpl(this, task_runner_, consumer));
  auto it_and_inserted = consumers_.emplace(endpoint.get());
  PERFETTO_DCHECK(it_and_inserted.second);
  task_runner_->PostTask(std::bind(&Consumer::OnConnect, endpoint->consumer()));
  return std::move(endpoint);
}

void ServiceImpl::DisconnectConsumer(ConsumerEndpointImpl* consumer) {
  PERFETTO_DCHECK(consumers_.count(consumer));
  // TODO: In next CL, tear down the trace sessions for the consumer.
  consumers_.erase(consumer);
}

void ServiceImpl::EnableTracing(ConsumerEndpointImpl*, const TraceConfig&) {
  PERFETTO_DLOG("not implemented yet");
}

void ServiceImpl::DisableTracing(ConsumerEndpointImpl*) {
  PERFETTO_DLOG("not implemented yet");
}

void ServiceImpl::ReadBuffers(ConsumerEndpointImpl*) {
  PERFETTO_DLOG("not implemented yet");
}

void ServiceImpl::FreeBuffers(ConsumerEndpointImpl*) {
  PERFETTO_DLOG("not implemented yet");
}

////////////////////////////////////////////////////////////////////////////////
// ServiceImpl::ConsumerEndpointImpl implementation
////////////////////////////////////////////////////////////////////////////////

ServiceImpl::ConsumerEndpointImpl::ConsumerEndpointImpl(ServiceImpl* service,
                                                        base::TaskRunner*,
                                                        Consumer* consumer)
    : service_(service), consumer_(consumer), weak_ptr_factory_(this) {}

ServiceImpl::ConsumerEndpointImpl::~ConsumerEndpointImpl() {
  consumer_->OnDisconnect();
  service_->DisconnectConsumer(this);
}

void ServiceImpl::ConsumerEndpointImpl::EnableTracing(const TraceConfig& cfg) {
  service_->EnableTracing(this, cfg);
}

void ServiceImpl::ConsumerEndpointImpl::DisableTracing() {
  service_->DisableTracing(this);
}

void ServiceImpl::ConsumerEndpointImpl::ReadBuffers() {
  service_->ReadBuffers(this);
}

void ServiceImpl::ConsumerEndpointImpl::FreeBuffers() {
  service_->FreeBuffers(this);
}

base::WeakPtr<ServiceImpl::ConsumerEndpointImpl>
ServiceImpl::ConsumerEndpointImpl::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
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
  producer_->OnDisconnect();
  service_->DisconnectProducer(id_);
}

void ServiceImpl::ProducerEndpointImpl::RegisterDataSource(
    const DataSourceDescriptor&,
    RegisterDataSourceCallback callback) {
  const DataSourceID dsid = ++last_data_source_id_;
  task_runner_->PostTask(std::bind(std::move(callback), dsid));
  // TODO implement the bookkeeping logic.
  if (service_->observer_)
    service_->observer_->OnDataSourceRegistered(id_, dsid);
}

void ServiceImpl::ProducerEndpointImpl::UnregisterDataSource(
    DataSourceID dsid) {
  PERFETTO_CHECK(dsid);
  // TODO implement the bookkeeping logic.
  if (service_->observer_)
    service_->observer_->OnDataSourceUnregistered(id_, dsid);
}

void ServiceImpl::ProducerEndpointImpl::NotifySharedMemoryUpdate(
    const std::vector<uint32_t>& changed_pages) {
  // TODO implement the bookkeeping logic.
  return;
}

void ServiceImpl::set_observer_for_testing(ObserverForTesting* observer) {
  observer_ = observer;
}

SharedMemory* ServiceImpl::ProducerEndpointImpl::shared_memory() const {
  return shared_memory_.get();
}

}  // namespace perfetto
