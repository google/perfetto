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

#ifndef INCLUDE_PERFETTO_TRACING_DATA_SOURCE_H_
#define INCLUDE_PERFETTO_TRACING_DATA_SOURCE_H_

// This header contains the key class (DataSource) that a producer app should
// override in order to create a custom data source that gets tracing Start/Stop
// notifications and emits tracing data.

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "perfetto/base/compiler.h"
#include "perfetto/base/export.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/message_handle.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/tracing/internal/basic_types.h"
#include "perfetto/tracing/internal/data_source_internal.h"
#include "perfetto/tracing/internal/tracing_muxer.h"
#include "perfetto/tracing/locked_handle.h"
#include "perfetto/tracing/trace_writer_base.h"

namespace perfetto {

class DataSourceConfig;

// Base class with the virtual methods to get start/stop notifications.
// Embedders are supposed to derive the templated version below, not this one.
class PERFETTO_EXPORT DataSourceBase {
 public:
  virtual ~DataSourceBase();

  // OnSetup() is invoked when tracing is configured. In most cases this happens
  // just before starting the trace. In the case of deferred start (see
  // deferred_start in trace_config.proto) start might happen later.
  struct SetupArgs {
    // This is valid only within the scope of the OnSetup() call and must not
    // be retained.
    const DataSourceConfig* config = nullptr;
  };
  virtual void OnSetup(const SetupArgs&);

  struct StartArgs {};
  virtual void OnStart(const StartArgs&);

  struct StopArgs {};
  virtual void OnStop(const StopArgs&);
};

// Templated base class meant to be derived by embedders to create a custom data
// source. DataSourceType must be the type of the derived class itself, e.g.:
// class MyDataSource : public DataSourceBase<MyDataSource> {...}.
template <typename DataSourceType>
class DataSource : public DataSourceBase {
 public:
  // Argument passed to the lambda function passed to Trace() (below).
  class TraceContext {
   public:
    using TracePacketHandle =
        ::protozero::MessageHandle<::perfetto::protos::pbzero::TracePacket>;

    TraceContext(TraceContext&&) noexcept = default;
    ~TraceContext() = default;

    TracePacketHandle NewTracePacket() {
      return trace_writer_->NewTracePacket();
    }

    // Returns a RAII handle to access the data source instance, guaranteeing
    // that it won't be deleted on another thread (because of trace stopping)
    // while accessing it from within the Trace() lambda.
    // The returned handle can be invalid (nullptr) if tracing is stopped
    // immediately before calling this. The caller is supposed to check for its
    // validity before using it. After checking, the handle is guaranteed to
    // remain valid until the handle goes out of scope.
    LockedHandle<DataSourceType> GetDataSourceLocked() {
      auto* internal_state = static_state_.TryGet(instance_index_);
      if (!internal_state)
        return LockedHandle<DataSourceType>();
      return LockedHandle<DataSourceType>(
          &internal_state->lock,
          static_cast<DataSourceType*>(internal_state->data_source.get()));
    }

   private:
    friend class DataSource;
    TraceContext(TraceWriterBase* trace_writer, uint32_t instance_index)
        : trace_writer_(trace_writer), instance_index_(instance_index) {}
    TraceContext(const TraceContext&) = delete;
    TraceContext& operator=(const TraceContext&) = delete;

    TraceWriterBase* const trace_writer_;
    uint32_t const instance_index_;
  };

  // The main tracing method. Tracing code should call this passing a lambda as
  // argument, with the following signature: void(TraceContext).
  // The lambda will be called synchronously (i.e., always before Trace()
  // returns) only if tracing is enabled and the data source has been enabled in
  // the tracing config.
  // The lambda can be called more than once per Trace() call, in the case of
  // concurrent tracing sessions (or even if the data source is instantiated
  // twice within the same trace config).
  template <typename Lambda>
  static void Trace(Lambda tracing_fn) {
    constexpr auto kMaxDataSourceInstances = internal::kMaxDataSourceInstances;

    // |instances| is a per-class bitmap that tells:
    // 1. If the data source is enabled at all.
    // 2. The index of the slot within |valid_instances| that holds the instance
    //    state. In turn this allows to map the data source to the tracing
    //    session and buffers.
    // memory_order_relaxed is okay because:
    // - |instances| is re-read with an acquire barrier below if this succeeds.
    // - The code between this point and the acquire-load is based on static
    //    storage which has indefinite lifetime.
    auto instances =
        static_state_.valid_instances.load(std::memory_order_relaxed);

    // This is the tracing fast-path. Bail out immediately if tracing is not
    // enabled (or tracing is enabled but not for this data source).
    if (PERFETTO_LIKELY(!instances))
      return;

    // TODO(primiano): all the stuff below should be outlined. Or at least
    // we should have some compile-time traits like kOptimizeBinarySize /
    // kOptimizeTracingLatency.

    // See tracing_muxer.h for the structure of the TLS.
    auto* tracing_impl = internal::TracingMuxer::Get();
    if (PERFETTO_UNLIKELY(!tls_state_))
      tls_state_ = tracing_impl->GetOrCreateDataSourceTLS(&static_state_);

    // TracingTLS::generation is a global monotonic counter that is incremented
    // every time a tracing session is stopped. We use that as a signal to force
    // a slow-path garbage collection of all the trace writers for the current
    // thread and to destroy the ones that belong to tracing sessions that have
    // ended. This is to avoid having too many TraceWriter instances alive, each
    // holding onto one chunk of the shared memory buffer.
    // Rationale why memory_order_relaxed should be fine:
    // - The TraceWriter object that we use is always constructed and destructed
    //   on the current thread. There is no risk of accessing a half-initialized
    //   TraceWriter (which would be really bad).
    // - In the worst case, in the case of a race on the generation check, we
    //   might end up using a TraceWriter for the same data source that belongs
    //   to a stopped session. This is not really wrong, as we don't give any
    //   guarantee on the global atomicity of the stop. In the worst case the
    //   service will reject the data commit if this arrives too late.

    if (PERFETTO_UNLIKELY(
            tls_state_->root_tls->generation !=
            tracing_impl->generation(std::memory_order_relaxed))) {
      // Will update root_tls->generation.
      tracing_impl->DestroyStoppedTraceWritersForCurrentThread();
    }

    for (uint32_t i = 0; i < kMaxDataSourceInstances; i++) {
      internal::DataSourceState* instance_state =
          static_state_.TryGetCached(instances, i);
      if (!instance_state)
        continue;

      // Even if we passed the check above, the DataSourceInstance might be
      // still destroyed concurrently while this code runs. The code below is
      // designed to deal with such race, as follows:
      // - We don't access the user-defined data source instance state. The only
      //   bits of state we use are |backend_id| and |buffer_id|.
      // - Beyond those two integers, we access only the TraceWriter here. The
      //   TraceWriter is always safe because it lives on the TLS.
      // - |instance_state| is backed by static storage, so the pointer is
      //   always valid, even after the data source instance is destroyed.
      // - In the case of a race-on-destruction, we'll still see the latest
      //   backend_id and buffer_id and in the worst case keep trying writing
      //   into the tracing shared memory buffer after stopped. But this isn't
      //   really any worse than the case of the stop IPC being delayed by the
      //   kernel scheduler. The tracing service is robust against data commit
      //   attemps made after tracing is stopped.
      // There is a theoretical race that would case the wrong behavior w.r.t
      // writing data in the wrong buffer, but it's so rare that we ignore it:
      // if the data source is stopped and started kMaxDataSourceInstances
      // times (so that the same id is recycled) while we are in this function,
      // we might end up reusing the old data source's backend_id and buffer_id
      // for the new one, because we don't see the generation change past this
      // point. But stopping and starting tracing (even once) takes so much
      // handshaking to make this extremely unrealistic.

      auto& tls_inst = tls_state_->per_instance[i];
      TraceWriterBase* trace_writer = tls_inst.trace_writer.get();

      if (PERFETTO_UNLIKELY(!trace_writer)) {
        // Here we need an acquire barrier, which matches the release-store made
        // by TracingMuxerImpl::SetupDataSource(), to ensure that the backend_id
        // and buffer_id are consistent.
        instances =
            static_state_.valid_instances.load(std::memory_order_acquire);
        instance_state = static_state_.TryGetCached(instances, i);
        if (!instance_state || !instance_state->started)
          return;
        tls_inst.backend_id = instance_state->backend_id;
        tls_inst.buffer_id = instance_state->buffer_id;
        tls_inst.trace_writer = tracing_impl->CreateTraceWriter(instance_state);
        trace_writer = tls_inst.trace_writer.get();

        // Even in the case of out-of-IDs, SharedMemoryArbiterImpl returns a
        // NullTraceWriter. The returned pointer should never be null.
        assert(trace_writer);
      }

      tracing_fn(TraceContext(trace_writer, i));
    }
  }

  // Registers the data source on all tracing backends, including ones that
  // connect after the registration. Doing so enables the data source to receive
  // Setup/Start/Stop notifications and makes the Trace() method work when
  // tracing is enabled and the data source is selected.
  // This must be called after Tracing::Initialize().
  // The caller must also use the DEFINE_DATA_SOURCE_STATIC_MEMBERS() macro
  // documented below.
  // Can return false to signal failure if attemping to register more than
  // kMaxDataSources (32) data sources types.
  static bool Register(const DataSourceDescriptor& descriptor) {
    // Silences -Wunused-variable warning in case the trace method is not used
    // by the translation unit that declares the data source.
    (void)static_state_;
    (void)tls_state_;

    auto factory = [] {
      return std::unique_ptr<DataSourceBase>(new DataSourceType());
    };
    auto* tracing_impl = internal::TracingMuxer::Get();
    return tracing_impl->RegisterDataSource(descriptor, factory,
                                            &static_state_);
  }

  // Static state. Accessed by the static Trace() method fastpaths.
  static internal::DataSourceStaticState static_state_;

  // This TLS object is a cached raw pointer and has deliberately no destructor.
  // The Platform implementation is supposed to create and manage the lifetime
  // of the Platform::ThreadLocalObject and take care of destroying it.
  // This is because non-POD thread_local variables have subtleties (global
  // destructors) that we need to defer to the embedder. In chromium's platform
  // implementation, for instance, the tls slot is implemented using
  // chromium's base::ThreadLocalStorage.
  static thread_local internal::DataSourceThreadLocalState* tls_state_;
};

}  // namespace perfetto

// The API client must use this in a translation unit. This is because it needs
// to instantiate the static storage for the datasource to allow the fastpath
// enabled check.
#define PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(X)          \
  template <>                                                  \
  perfetto::internal::DataSourceStaticState                    \
      perfetto::DataSource<X>::static_state_{};                \
  template <>                                                  \
  thread_local perfetto::internal::DataSourceThreadLocalState* \
      perfetto::DataSource<X>::tls_state_ = nullptr

#endif  // INCLUDE_PERFETTO_TRACING_DATA_SOURCE_H_
