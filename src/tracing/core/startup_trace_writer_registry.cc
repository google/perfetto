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

#include "perfetto/ext/tracing/core/startup_trace_writer_registry.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/tracing/core/startup_trace_writer.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"

using ChunkHeader = perfetto::SharedMemoryABI::ChunkHeader;

namespace perfetto {

StartupTraceWriterRegistryHandle::StartupTraceWriterRegistryHandle(
    StartupTraceWriterRegistry* registry)
    : registry_(registry) {}

void StartupTraceWriterRegistryHandle::ReturnWriterToRegistry(
    std::unique_ptr<StartupTraceWriter> writer) {
  std::lock_guard<std::mutex> lock(lock_);
  if (registry_)
    registry_->ReturnTraceWriter(std::move(writer));
}

void StartupTraceWriterRegistryHandle::OnRegistryDestroyed() {
  std::lock_guard<std::mutex> lock(lock_);
  registry_ = nullptr;
}

StartupTraceWriterRegistry::StartupTraceWriterRegistry()
    : handle_(std::make_shared<StartupTraceWriterRegistryHandle>(this)) {}

StartupTraceWriterRegistry::~StartupTraceWriterRegistry() {
  handle_->OnRegistryDestroyed();
}

// static
constexpr size_t StartupTraceWriterRegistry::kDefaultMaxBufferSizeBytes;

std::unique_ptr<StartupTraceWriter>
StartupTraceWriterRegistry::CreateUnboundTraceWriter(
    BufferExhaustedPolicy buffer_exhausted_policy,
    size_t max_buffer_size_bytes) {
  std::lock_guard<std::mutex> lock(lock_);
  PERFETTO_DCHECK(!arbiter_);  // Should only be called while unbound.
  std::unique_ptr<StartupTraceWriter> writer(new StartupTraceWriter(
      handle_, buffer_exhausted_policy, max_buffer_size_bytes));
  unbound_writers_.push_back(writer.get());
  return writer;
}

void StartupTraceWriterRegistry::ReturnTraceWriter(
    std::unique_ptr<StartupTraceWriter> trace_writer) {
  std::unique_lock<std::mutex> lock(lock_);

  // We can only bind the writer on task_runner_.
  if (task_runner_ && !task_runner_->RunsTasksOnCurrentThread()) {
    // We shouldn't post tasks while holding a lock. |task_runner_| is only set
    // once, so will remain valid.
    lock.unlock();
    auto weak_this = weak_ptr_factory_->GetWeakPtr();
    auto* trace_writer_raw = trace_writer.release();
    task_runner_->PostTask([weak_this, trace_writer_raw]() {
      std::unique_ptr<StartupTraceWriter> owned_writer(trace_writer_raw);
      if (weak_this)
        weak_this->ReturnTraceWriter(std::move(owned_writer));
    });
    return;
  }

  PERFETTO_DCHECK(!trace_writer->write_in_progress_);
  auto it = std::find(unbound_writers_.begin(), unbound_writers_.end(),
                      trace_writer.get());

  // If the registry is already bound, but the writer wasn't, bind it now.
  if (arbiter_) {
    if (it == unbound_writers_.end()) {
      // Nothing to do, the writer was already bound.
      return;
    }

    // This should succeed since nobody can write to this writer concurrently.
    bool success = trace_writer->BindToArbiter(arbiter_, target_buffer_,
                                               chunks_per_batch_);
    PERFETTO_DCHECK(success);
    unbound_writers_.erase(it);

    OnUnboundWritersRemovedLocked();
    return;
  }

  // If the registry was not bound yet, keep the writer alive until it is.
  PERFETTO_DCHECK(it != unbound_writers_.end());
  unbound_writers_.erase(it);
  unbound_owned_writers_.push_back(std::move(trace_writer));
}

void StartupTraceWriterRegistry::BindToArbiter(
    SharedMemoryArbiterImpl* arbiter,
    BufferID target_buffer,
    base::TaskRunner* task_runner,
    std::function<void(StartupTraceWriterRegistry*)> on_bound_callback) {
  std::vector<std::unique_ptr<StartupTraceWriter>> unbound_owned_writers;
  {
    std::lock_guard<std::mutex> lock(lock_);
    PERFETTO_DCHECK(!arbiter_);
    arbiter_ = arbiter;
    target_buffer_ = target_buffer;
    task_runner_ = task_runner;

    // Attempt to use at most half the SMB for binding of StartupTraceWriters at
    // the same time. In the worst case, all writers are binding at the same
    // time, so divide it up between them.
    //
    // TODO(eseckler): This assumes that there's only a single registry at the
    // same time. SharedMemoryArbiterImpl should advise us how much of the SMB
    // we're allowed to use in the first place.
    size_t num_writers =
        unbound_owned_writers_.size() + unbound_writers_.size();
    if (num_writers) {
      chunks_per_batch_ = arbiter_->num_pages() / 2 / num_writers;
    } else {
      chunks_per_batch_ = arbiter_->num_pages() / 2;
    }
    // We should use at least one chunk per batch.
    chunks_per_batch_ = std::max(chunks_per_batch_, static_cast<size_t>(1u));

    // Weakptrs should be valid on |task_runner|. For this, the factory needs to
    // be created on |task_runner|, i.e. BindToArbiter must be called on
    // |task_runner|.
    PERFETTO_DCHECK(task_runner_->RunsTasksOnCurrentThread());
    weak_ptr_factory_.reset(
        new base::WeakPtrFactory<StartupTraceWriterRegistry>(this));
    on_bound_callback_ = std::move(on_bound_callback);
    // We can't destroy the writers while holding |lock_|, so we swap them out
    // here instead. After we are bound, no more writers can be added to the
    // list.
    unbound_owned_writers.swap(unbound_owned_writers_);
  }

  // Bind and destroy the owned writers.
  for (const auto& writer : unbound_owned_writers) {
    // This should succeed since nobody can write to these writers concurrently.
    bool success =
        writer->BindToArbiter(arbiter_, target_buffer_, chunks_per_batch_);
    PERFETTO_DCHECK(success);
  }
  unbound_owned_writers.clear();

  TryBindWriters();
}

void StartupTraceWriterRegistry::TryBindWriters() {
  std::lock_guard<std::mutex> lock(lock_);
  for (auto it = unbound_writers_.begin(); it != unbound_writers_.end();) {
    if ((*it)->BindToArbiter(arbiter_, target_buffer_, chunks_per_batch_)) {
      it = unbound_writers_.erase(it);
    } else {
      break;
    }
  }
  if (!unbound_writers_.empty()) {
    auto weak_this = weak_ptr_factory_->GetWeakPtr();
    task_runner_->PostTask([weak_this] {
      if (weak_this)
        weak_this->TryBindWriters();
    });
  }
  OnUnboundWritersRemovedLocked();
}

void StartupTraceWriterRegistry::OnUnboundWritersRemovedLocked() {
  if (!unbound_writers_.empty() || !task_runner_ || !on_bound_callback_)
    return;

  PERFETTO_DCHECK(weak_ptr_factory_);
  auto weak_this = weak_ptr_factory_->GetWeakPtr();
  // Run callback in PostTask() since the callback may delete |this| and thus
  // might otherwise cause a deadlock.
  auto callback = on_bound_callback_;
  on_bound_callback_ = nullptr;
  task_runner_->PostTask([weak_this, callback]() {
    if (!weak_this)
      return;
    // Note: callback may delete |this|.
    callback(weak_this.get());
  });
}

}  // namespace perfetto
