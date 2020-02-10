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

#ifndef INCLUDE_PERFETTO_EXT_TRACING_CORE_STARTUP_TRACE_WRITER_REGISTRY_H_
#define INCLUDE_PERFETTO_EXT_TRACING_CORE_STARTUP_TRACE_WRITER_REGISTRY_H_

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "perfetto/base/export.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"

namespace perfetto {

class SharedMemoryArbiterImpl;
class StartupTraceWriter;
class StartupTraceWriterRegistry;

namespace base {
class TaskRunner;
}  // namespace base

// Used to return a StartupTraceWriter back to its registry when the writer's
// thread is destroyed, provided the registry itself wasn't deleted yet. The
// indirection via the handle is necessary to avoid potential deadlocks caused
// by lock order inversion. These issues are avoided by locking on the handle's
// common lock.
class StartupTraceWriterRegistryHandle {
 public:
  explicit StartupTraceWriterRegistryHandle(StartupTraceWriterRegistry*);

  // Called by StartupTraceWriterRegistry destructor.
  void OnRegistryDestroyed();

  // Called by StartupTraceWriter::ReturnToRegistry.
  void ReturnWriterToRegistry(std::unique_ptr<StartupTraceWriter> writer);

 private:
  StartupTraceWriterRegistryHandle(const StartupTraceWriterRegistryHandle&) =
      delete;
  StartupTraceWriterRegistryHandle& operator=(
      const StartupTraceWriterRegistryHandle&) = delete;

  std::mutex lock_;
  StartupTraceWriterRegistry* registry_;
};

// Embedders can use this registry to create unbound StartupTraceWriters during
// startup, and later bind them all safely to an arbiter and target buffer.
//
// DEPRECATED. See SharedMemoryArbiter::CreateUnboundInstance() for a
// replacement.
//
// TODO(eseckler): Remove StartupTraceWriter support.
class PERFETTO_EXPORT StartupTraceWriterRegistry {
 public:
  StartupTraceWriterRegistry();
  ~StartupTraceWriterRegistry();

  // Buffer size defaults to 1 mB per writer.
  static constexpr size_t kDefaultMaxBufferSizeBytes = 1024 * 1024;

  // Returns a new unbound StartupTraceWriter. Should only be called while
  // unbound. Usually called on a writer thread. The writer should never be
  // destroyed by the caller directly, but instead returned to the registry by
  // calling StartupTraceWriter::ReturnToRegistry.
  std::unique_ptr<StartupTraceWriter> CreateUnboundTraceWriter(
      BufferExhaustedPolicy = BufferExhaustedPolicy::kDefault,
      size_t max_buffer_size_bytes = kDefaultMaxBufferSizeBytes);

  // Binds all StartupTraceWriters created by this registry to the given arbiter
  // and target buffer. Should only be called once and on the passed
  // TaskRunner's sequence. See
  // SharedMemoryArbiter::BindStartupTraceWriterRegistry() for details.
  //
  // Note that the writers may not be bound synchronously if they are
  // concurrently being written to. The registry will retry on the passed
  // TaskRunner until all writers were bound successfully.
  //
  // Calls |on_bound_callback| asynchronously on the passed TaskRunner once all
  // writers were bound.
  //
  // The commit of the StartupTraceWriters' locally buffered data to the SMB is
  // rate limited to avoid exhausting the SMB, and may continue asynchronously
  // even after |on_bound_callback| was called.
  void BindToArbiter(
      SharedMemoryArbiterImpl*,
      BufferID target_buffer,
      base::TaskRunner*,
      std::function<void(StartupTraceWriterRegistry*)> on_bound_callback);

 private:
  friend class StartupTraceWriterRegistryHandle;
  friend class StartupTraceWriterTest;

  StartupTraceWriterRegistry(const StartupTraceWriterRegistry&) = delete;
  StartupTraceWriterRegistry& operator=(const StartupTraceWriterRegistry&) =
      delete;

  // Try to bind the remaining unbound writers and post a continuation to
  // |task_runner_| if any writers could not be bound.
  void TryBindWriters();

  // Notifies the arbiter when we have bound all writers. May delete |this|.
  void OnUnboundWritersRemovedLocked();

  // Return a StartupTraceWriter back to the registry, see
  // StartupTraceWriter::ReturnToRegistry.
  void ReturnTraceWriter(std::unique_ptr<StartupTraceWriter>);

  std::shared_ptr<StartupTraceWriterRegistryHandle> handle_;

  // Begin lock-protected members.
  std::mutex lock_;

  // Unbound writers that we handed out to writer threads. These writers may be
  // concurrently written to by the writer threads.
  std::vector<StartupTraceWriter*> unbound_writers_;

  // Unbound writers that writer threads returned to the registry by calling
  // ReturnUnboundTraceWriter(). Writers are removed from |unbound_writers_|
  // when they are added to |unbound_owned_writers_|. No new data can be written
  // to these writers.
  std::vector<std::unique_ptr<StartupTraceWriter>> unbound_owned_writers_;

  SharedMemoryArbiterImpl* arbiter_ = nullptr;  // |nullptr| while unbound.
  BufferID target_buffer_ = 0;
  base::TaskRunner* task_runner_ = nullptr;
  size_t chunks_per_batch_ = 0;
  std::function<void(StartupTraceWriterRegistry*)> on_bound_callback_ = nullptr;

  // Keep at the end. Initialized during |BindToArbiter()|, like |task_runner_|.
  // Weak pointers are only valid on |task_runner_|'s thread/sequence.
  std::unique_ptr<base::WeakPtrFactory<StartupTraceWriterRegistry>>
      weak_ptr_factory_;
  // End lock-protected members.
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_TRACING_CORE_STARTUP_TRACE_WRITER_REGISTRY_H_
