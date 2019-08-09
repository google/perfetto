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

#include "src/tracing/internal/tracing_muxer_impl.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/thread_checker.h"
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/ext/tracing/core/buffer_exhausted_policy.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/internal/data_source_internal.h"
#include "perfetto/tracing/trace_writer_base.h"
#include "perfetto/tracing/tracing.h"
#include "perfetto/tracing/tracing_backend.h"
#include "src/tracing/internal/in_process_tracing_backend.h"
#include "src/tracing/internal/system_tracing_backend.h"

namespace perfetto {
namespace internal {

namespace {

class StopArgsImpl : public DataSourceBase::StopArgs {
 public:
  std::function<void()> HandleStopAsynchronously() const override {
    auto closure = std::move(async_stop_closure);
    async_stop_closure = std::function<void()>();
    return closure;
  }

  mutable std::function<void()> async_stop_closure;
};

}  // namespace

// ----- Begin of TracingMuxerImpl::ProducerImpl
TracingMuxerImpl::ProducerImpl::ProducerImpl(TracingMuxerImpl* muxer,
                                             TracingBackendId backend_id)
    : muxer_(muxer), backend_id_(backend_id) {}
TracingMuxerImpl::ProducerImpl::~ProducerImpl() = default;

void TracingMuxerImpl::ProducerImpl::Initialize(
    std::unique_ptr<ProducerEndpoint> endpoint) {
  service_ = std::move(endpoint);
}

void TracingMuxerImpl::ProducerImpl::OnConnect() {
  PERFETTO_DLOG("Producer connected");
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DCHECK(!connected_);
  connected_ = true;
  muxer_->UpdateDataSourcesOnAllBackends();
}

void TracingMuxerImpl::ProducerImpl::OnDisconnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  connected_ = false;
  PERFETTO_DFATAL("Producer::OnDisconnect not implemented");  // TODO.
}

void TracingMuxerImpl::ProducerImpl::OnTracingSetup() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
}

void TracingMuxerImpl::ProducerImpl::SetupDataSource(
    DataSourceInstanceID id,
    const DataSourceConfig& cfg) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  muxer_->SetupDataSource(backend_id_, id, cfg);
}

void TracingMuxerImpl::ProducerImpl::StartDataSource(DataSourceInstanceID id,
                                                     const DataSourceConfig&) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  muxer_->StartDataSource(backend_id_, id);
  service_->NotifyDataSourceStarted(id);
}

void TracingMuxerImpl::ProducerImpl::StopDataSource(DataSourceInstanceID id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  muxer_->StopDataSource_AsyncBegin(backend_id_, id);
}

void TracingMuxerImpl::ProducerImpl::Flush(FlushRequestID flush_id,
                                           const DataSourceInstanceID*,
                                           size_t) {
  // Flush is not plumbed for now, we just ack straight away.
  PERFETTO_DCHECK_THREAD(thread_checker_);
  service_->NotifyFlushComplete(flush_id);
}

void TracingMuxerImpl::ProducerImpl::ClearIncrementalState(
    const DataSourceInstanceID*,
    size_t) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
}
// ----- End of TracingMuxerImpl::ProducerImpl methods.

// ----- Begin of TracingMuxerImpl::ConsumerImpl
TracingMuxerImpl::ConsumerImpl::ConsumerImpl(TracingMuxerImpl* muxer,
                                             TracingBackendId backend_id,
                                             TracingSessionGlobalID session_id)
    : muxer_(muxer), backend_id_(backend_id), session_id_(session_id) {}

TracingMuxerImpl::ConsumerImpl::~ConsumerImpl() = default;

void TracingMuxerImpl::ConsumerImpl::Initialize(
    std::unique_ptr<ConsumerEndpoint> endpoint) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  service_ = std::move(endpoint);
  // Observe data source instance events so we get notified when tracing starts.
  service_->ObserveEvents(ConsumerEndpoint::kDataSourceInstances);
}

void TracingMuxerImpl::ConsumerImpl::OnConnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DCHECK(!connected_);
  connected_ = true;

  // If the API client configured and started tracing before we connected,
  // tell the backend about it now.
  if (trace_config_) {
    muxer_->SetupTracingSession(session_id_, trace_config_);
    if (start_pending_)
      muxer_->StartTracingSession(session_id_);
    if (stop_pending_)
      muxer_->StopTracingSession(session_id_);
  }
}

void TracingMuxerImpl::ConsumerImpl::OnDisconnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // It shouldn't be necessary to call StopTracingSession. If we get this call
  // it means that the service did shutdown before us, so there is no point
  // trying it to ask it to stop the session. We should just remember to cleanup
  // the consumer vector.
  connected_ = false;

  // TODO notify the client somehow.
}

void TracingMuxerImpl::ConsumerImpl::OnTracingDisabled() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DCHECK(!stopped_);
  stopped_ = true;
  // If we're still waiting for the start event, fire it now. This may happen if
  // there are no active data sources in the session.
  NotifyStartComplete();
  NotifyStopComplete();
}

void TracingMuxerImpl::ConsumerImpl::NotifyStartComplete() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (blocking_start_complete_callback_) {
    muxer_->task_runner_->PostTask(
        std::move(blocking_start_complete_callback_));
    blocking_start_complete_callback_ = nullptr;
  }
}

void TracingMuxerImpl::ConsumerImpl::NotifyStopComplete() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (stop_complete_callback_) {
    muxer_->task_runner_->PostTask(std::move(stop_complete_callback_));
    stop_complete_callback_ = nullptr;
  }
  if (blocking_stop_complete_callback_) {
    muxer_->task_runner_->PostTask(std::move(blocking_stop_complete_callback_));
    blocking_stop_complete_callback_ = nullptr;
  }
}

void TracingMuxerImpl::ConsumerImpl::OnTraceData(
    std::vector<TracePacket> packets,
    bool has_more) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (!read_trace_callback_)
    return;

  size_t capacity = 0;
  for (const auto& packet : packets) {
    // 16 is an over-estimation of the proto preamble size
    capacity += packet.size() + 16;
  }

  // The shared_ptr is to avoid making a copy of the buffer when PostTask-ing.
  std::shared_ptr<std::vector<char>> buf(new std::vector<char>());
  buf->reserve(capacity);
  for (auto& packet : packets) {
    char* start;
    size_t size;
    std::tie(start, size) = packet.GetProtoPreamble();
    buf->insert(buf->end(), start, start + size);
    for (auto& slice : packet.slices()) {
      const auto* slice_data = reinterpret_cast<const char*>(slice.start);
      buf->insert(buf->end(), slice_data, slice_data + slice.size);
    }
  }

  auto callback = read_trace_callback_;
  muxer_->task_runner_->PostTask([callback, buf, has_more] {
    TracingSession::ReadTraceCallbackArgs callback_arg{};
    callback_arg.data = &(*buf)[0];
    callback_arg.size = buf->size();
    callback_arg.has_more = has_more;
    callback(callback_arg);
  });

  if (!has_more)
    read_trace_callback_ = nullptr;
}

void TracingMuxerImpl::ConsumerImpl::OnObservableEvents(
    const ObservableEvents& events) {
  if (events.instance_state_changes_size()) {
    for (const auto& state_change : events.instance_state_changes()) {
      DataSourceHandle handle{state_change.producer_name(),
                              state_change.data_source_name()};
      data_source_states_[handle] =
          state_change.state() ==
          ObservableEvents::DataSourceInstanceStateChange::
              DATA_SOURCE_INSTANCE_STATE_STARTED;
    }
    // Data sources are first reported as being stopped before starting, so once
    // all the data sources we know about have started we can declare tracing
    // begun.
    if (blocking_start_complete_callback_) {
      bool all_data_sources_started = std::all_of(
          data_source_states_.cbegin(), data_source_states_.cend(),
          [](std::pair<DataSourceHandle, bool> state) { return state.second; });
      if (all_data_sources_started)
        NotifyStartComplete();
    }
  }
}

// The callbacks below are not used.
void TracingMuxerImpl::ConsumerImpl::OnDetach(bool) {}
void TracingMuxerImpl::ConsumerImpl::OnAttach(bool, const TraceConfig&) {}
void TracingMuxerImpl::ConsumerImpl::OnTraceStats(bool, const TraceStats&) {}
// ----- End of TracingMuxerImpl::ConsumerImpl

// ----- Begin of TracingMuxerImpl::TracingSessionImpl

// TracingSessionImpl is the RAII object returned to API clients when they
// invoke Tracing::CreateTracingSession. They use it for starting/stopping
// tracing.

TracingMuxerImpl::TracingSessionImpl::TracingSessionImpl(
    TracingMuxerImpl* muxer,
    TracingSessionGlobalID session_id)
    : muxer_(muxer), session_id_(session_id) {}

// Can be destroyed from any thread.
TracingMuxerImpl::TracingSessionImpl::~TracingSessionImpl() {
  auto* muxer = muxer_;
  auto session_id = session_id_;
  muxer->task_runner_->PostTask(
      [muxer, session_id] { muxer->DestroyTracingSession(session_id); });
}

// Can be called from any thread.
void TracingMuxerImpl::TracingSessionImpl::Setup(const TraceConfig& cfg,
                                                 int fd) {
  auto* muxer = muxer_;
  auto session_id = session_id_;
  std::shared_ptr<TraceConfig> trace_config(new TraceConfig(cfg));
  if (fd >= 0) {
    trace_config->set_write_into_file(true);
    fd = dup(fd);
  }
  muxer->task_runner_->PostTask([muxer, session_id, trace_config, fd] {
    muxer->SetupTracingSession(session_id, trace_config, base::ScopedFile(fd));
  });
}

// Can be called from any thread.
void TracingMuxerImpl::TracingSessionImpl::Start() {
  auto* muxer = muxer_;
  auto session_id = session_id_;
  muxer->task_runner_->PostTask(
      [muxer, session_id] { muxer->StartTracingSession(session_id); });
}

// Can be called from any thread except the service thread.
void TracingMuxerImpl::TracingSessionImpl::StartBlocking() {
  PERFETTO_DCHECK(!muxer_->task_runner_->RunsTasksOnCurrentThread());
  auto* muxer = muxer_;
  auto session_id = session_id_;
  base::WaitableEvent tracing_started;
  muxer->task_runner_->PostTask([muxer, session_id, &tracing_started] {
    auto* consumer = muxer->FindConsumer(session_id);
    PERFETTO_DCHECK(!consumer->blocking_start_complete_callback_);
    consumer->blocking_start_complete_callback_ = [&] {
      tracing_started.Notify();
    };
    muxer->StartTracingSession(session_id);
  });
  tracing_started.Wait();
}

// Can be called from any thread.
void TracingMuxerImpl::TracingSessionImpl::Stop() {
  auto* muxer = muxer_;
  auto session_id = session_id_;
  muxer->task_runner_->PostTask(
      [muxer, session_id] { muxer->StopTracingSession(session_id); });
}

// Can be called from any thread except the service thread.
void TracingMuxerImpl::TracingSessionImpl::StopBlocking() {
  PERFETTO_DCHECK(!muxer_->task_runner_->RunsTasksOnCurrentThread());
  auto* muxer = muxer_;
  auto session_id = session_id_;
  base::WaitableEvent tracing_stopped;
  muxer->task_runner_->PostTask([muxer, session_id, &tracing_stopped] {
    auto* consumer = muxer->FindConsumer(session_id);
    PERFETTO_DCHECK(!consumer->blocking_stop_complete_callback_);
    consumer->blocking_stop_complete_callback_ = [&] {
      tracing_stopped.Notify();
    };
    muxer->StopTracingSession(session_id);
  });
  tracing_stopped.Wait();
}

// Can be called from any thread.
void TracingMuxerImpl::TracingSessionImpl::ReadTrace(ReadTraceCallback cb) {
  auto* muxer = muxer_;
  auto session_id = session_id_;
  muxer->task_runner_->PostTask([muxer, session_id, cb] {
    muxer->ReadTracingSessionData(session_id, std::move(cb));
  });
}

// Can be called from any thread.
void TracingMuxerImpl::TracingSessionImpl::SetOnStopCallback(
    std::function<void()> cb) {
  auto* muxer = muxer_;
  auto session_id = session_id_;
  muxer->task_runner_->PostTask([muxer, session_id, cb] {
    auto* consumer = muxer->FindConsumer(session_id);
    consumer->stop_complete_callback_ = cb;
  });
}
// ----- End of TracingMuxerImpl::TracingSessionImpl

// static
TracingMuxer* TracingMuxer::instance_ = nullptr;

// This is called by perfetto::Tracing::Initialize().
// Can be called on any thread. Typically, but not necessarily, that will be
// the embedder's main thread.
TracingMuxerImpl::TracingMuxerImpl(const TracingInitArgs& args)
    : TracingMuxer(args.platform ? args.platform
                                 : Platform::GetDefaultPlatform()) {
  PERFETTO_DETACH_FROM_THREAD(thread_checker_);

  // Create the thread where muxer, producers and service will live.
  task_runner_ = platform_->CreateTaskRunner({});

  // Run the initializer on that thread.
  task_runner_->PostTask([this, args] { Initialize(args); });
}

void TracingMuxerImpl::Initialize(const TracingInitArgs& args) {
  PERFETTO_DCHECK_THREAD(thread_checker_);  // Rebind the thread checker.

  auto add_backend = [this](TracingBackend* backend, BackendType type) {
    TracingBackendId backend_id = backends_.size();
    backends_.emplace_back();
    RegisteredBackend& rb = backends_.back();
    rb.backend = backend;
    rb.id = backend_id;
    rb.type = type;
    rb.producer.reset(new ProducerImpl(this, backend_id));
    TracingBackend::ConnectProducerArgs conn_args;
    conn_args.producer = rb.producer.get();
    conn_args.producer_name = platform_->GetCurrentProcessName();
    conn_args.task_runner = task_runner_.get();
    rb.producer->Initialize(rb.backend->ConnectProducer(conn_args));
  };

  if (args.backends & kSystemBackend) {
// These buildflags match the |perfetto_build_with_ipc_layer| condition in
// the //src/tracing:client_api target.
#if (PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) ||     \
     PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD) ||    \
     PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)) && \
    (PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||          \
     PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) ||        \
     PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX))
    add_backend(SystemTracingBackend::GetInstance(), kSystemBackend);
#else
    PERFETTO_ELOG("System backend not supporteed in the current configuration");
#endif
  }

  if (args.backends & kInProcessBackend)
    add_backend(InProcessTracingBackend::GetInstance(), kInProcessBackend);

  if (args.backends & kCustomBackend) {
    PERFETTO_CHECK(args.custom_backend);
    add_backend(args.custom_backend, kCustomBackend);
  }

  if (args.backends & ~(kSystemBackend | kInProcessBackend | kCustomBackend)) {
    PERFETTO_FATAL("Unsupported tracing backend type");
  }
}

// Can be called from any thread (but not concurrently).
bool TracingMuxerImpl::RegisterDataSource(
    const DataSourceDescriptor& descriptor,
    DataSourceFactory factory,
    DataSourceStaticState* static_state) {
  // Ignore repeated registrations.
  if (static_state->index != kMaxDataSources)
    return true;

  static std::atomic<uint32_t> last_id{};
  uint32_t new_index = last_id++;
  if (new_index >= kMaxDataSources - 1) {
    PERFETTO_DLOG(
        "RegisterDataSource failed: too many data sources already registered");
    return false;
  }

  // Initialize the static state.
  static_assert(sizeof(static_state->instances[0]) >= sizeof(DataSourceState),
                "instances[] size mismatch");
  for (size_t i = 0; i < static_state->instances.size(); i++)
    new (&static_state->instances[i]) DataSourceState{};

  static_state->index = new_index;

  task_runner_->PostTask([this, descriptor, factory, static_state] {
    data_sources_.emplace_back();
    RegisteredDataSource& rds = data_sources_.back();
    rds.descriptor = descriptor;
    rds.factory = factory;
    rds.static_state = static_state;
    UpdateDataSourcesOnAllBackends();
  });
  return true;
}

// Called by the service of one of the backends.
void TracingMuxerImpl::SetupDataSource(TracingBackendId backend_id,
                                       DataSourceInstanceID instance_id,
                                       const DataSourceConfig& cfg) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DLOG("Setting up data source %" PRIu64 " %s", instance_id,
                cfg.name().c_str());

  for (const auto& rds : data_sources_) {
    if (rds.descriptor.name() != cfg.name())
      continue;

    DataSourceStaticState& static_state = *rds.static_state;
    for (uint32_t i = 0; i < kMaxDataSourceInstances; i++) {
      // Find a free slot.
      if (static_state.TryGet(i))
        continue;

      auto* internal_state =
          reinterpret_cast<DataSourceState*>(&static_state.instances[i]);
      std::lock_guard<std::recursive_mutex> guard(internal_state->lock);
      static_assert(
          std::is_same<decltype(internal_state->data_source_instance_id),
                       DataSourceInstanceID>::value,
          "data_source_instance_id type mismatch");
      internal_state->backend_id = backend_id;
      internal_state->data_source_instance_id = instance_id;
      internal_state->buffer_id =
          static_cast<internal::BufferId>(cfg.target_buffer());
      internal_state->data_source = rds.factory();

      // This must be made at the end. See matching acquire-load in
      // DataSource::Trace().
      static_state.valid_instances.fetch_or(1 << i, std::memory_order_acq_rel);

      DataSourceBase::SetupArgs setup_args;
      setup_args.config = &cfg;
      internal_state->data_source->OnSetup(setup_args);
      return;
    }
    PERFETTO_ELOG(
        "Maximum number of data source instances exhausted. "
        "Dropping data source %" PRIu64,
        instance_id);
  }
}

// Called by the service of one of the backends.
void TracingMuxerImpl::StartDataSource(TracingBackendId backend_id,
                                       DataSourceInstanceID instance_id) {
  PERFETTO_DLOG("Starting data source %" PRIu64, instance_id);
  PERFETTO_DCHECK_THREAD(thread_checker_);

  auto ds = FindDataSource(backend_id, instance_id);
  if (!ds) {
    PERFETTO_ELOG("Could not find data source to start");
    return;
  }

  std::lock_guard<std::recursive_mutex> guard(ds.internal_state->lock);
  ds.internal_state->trace_lambda_enabled = true;
  ds.internal_state->data_source->OnStart(DataSourceBase::StartArgs{});
}

// Called by the service of one of the backends.
void TracingMuxerImpl::StopDataSource_AsyncBegin(
    TracingBackendId backend_id,
    DataSourceInstanceID instance_id) {
  PERFETTO_DLOG("Stopping data source %" PRIu64, instance_id);
  PERFETTO_DCHECK_THREAD(thread_checker_);

  auto ds = FindDataSource(backend_id, instance_id);
  if (!ds) {
    PERFETTO_ELOG("Could not find data source to stop");
    return;
  }

  StopArgsImpl stop_args{};
  stop_args.async_stop_closure = [this, backend_id, instance_id] {
    // TracingMuxerImpl is long lived, capturing |this| is okay.
    // The notification closure can be moved out of the StopArgs by the
    // embedder to handle stop asynchronously. The embedder might then
    // call the closure on a different thread than the current one, hence
    // this nested PostTask().
    task_runner_->PostTask([this, backend_id, instance_id] {
      StopDataSource_AsyncEnd(backend_id, instance_id);
    });
  };

  {
    std::lock_guard<std::recursive_mutex> guard(ds.internal_state->lock);
    ds.internal_state->data_source->OnStop(stop_args);
  }

  // If the embedder hasn't called StopArgs.HandleStopAsynchronously() run the
  // async closure here. In theory we could avoid the PostTask and call
  // straight into CompleteDataSourceAsyncStop(). We keep that to reduce
  // divergencies between the deferred-stop vs non-deferred-stop code paths.
  if (stop_args.async_stop_closure)
    std::move(stop_args.async_stop_closure)();
}

void TracingMuxerImpl::StopDataSource_AsyncEnd(
    TracingBackendId backend_id,
    DataSourceInstanceID instance_id) {
  PERFETTO_DLOG("Ending async stop of data source %" PRIu64, instance_id);
  PERFETTO_DCHECK_THREAD(thread_checker_);

  auto ds = FindDataSource(backend_id, instance_id);
  if (!ds) {
    PERFETTO_ELOG(
        "Async stop of data source %" PRIu64
        " failed. This might be due to calling the async_stop_closure twice.",
        instance_id);
    return;
  }

  const uint32_t mask = ~(1 << ds.instance_idx);
  ds.static_state->valid_instances.fetch_and(mask, std::memory_order_acq_rel);

  // Take the mutex to prevent that the data source is in the middle of
  // a Trace() execution where it called GetDataSourceLocked() while we
  // destroy it.
  {
    std::lock_guard<std::recursive_mutex> guard(ds.internal_state->lock);
    ds.internal_state->trace_lambda_enabled = false;
    ds.internal_state->data_source.reset();
  }

  // The other fields of internal_state are deliberately *not* cleared.
  // See races-related comments of DataSource::Trace().

  TracingMuxer::generation_++;

  // |backends_| is append-only, Backend instances are always valid.
  PERFETTO_CHECK(backend_id < backends_.size());
  ProducerImpl* producer = backends_[backend_id].producer.get();
  if (producer && producer->connected_)
    producer->service_->NotifyDataSourceStopped(instance_id);
}

void TracingMuxerImpl::DestroyStoppedTraceWritersForCurrentThread() {
  // Iterate across all possible data source types.
  auto cur_generation = generation_.load(std::memory_order_acquire);
  auto* root_tls = GetOrCreateTracingTLS();
  for (size_t ds_idx = 0; ds_idx < kMaxDataSources; ds_idx++) {
    // |tls| has a vector of per-data-source-instance thread-local state.
    DataSourceThreadLocalState& tls = root_tls->data_sources_tls[ds_idx];
    DataSourceStaticState* static_state = tls.static_state;
    if (!static_state)
      continue;  // Slot not used.

    // Iterate across all possible instances for this data source.
    for (uint32_t inst = 0; inst < kMaxDataSourceInstances; inst++) {
      DataSourceInstanceThreadLocalState& ds_tls = tls.per_instance[inst];
      if (!ds_tls.trace_writer)
        continue;

      DataSourceState* ds_state = static_state->TryGet(inst);
      if (ds_state && ds_state->backend_id == ds_tls.backend_id &&
          ds_state->buffer_id == ds_tls.buffer_id) {
        continue;
      }

      // The DataSource instance has been destroyed or recycled.
      ds_tls.Reset();  // Will also destroy the |ds_tls.trace_writer|.
    }
  }
  root_tls->generation = cur_generation;
}

// Called both when a new data source is registered or when a new backend
// connects. In both cases we want to be sure we reflected the data source
// registrations on the backends.
void TracingMuxerImpl::UpdateDataSourcesOnAllBackends() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (RegisteredDataSource& rds : data_sources_) {
    for (RegisteredBackend& backend : backends_) {
      // We cannot call RegisterDataSource on the backend before it connects.
      if (!backend.producer->connected_)
        continue;

      PERFETTO_DCHECK(rds.static_state->index < kMaxDataSourceInstances);
      if (backend.producer->registered_data_sources_.test(
              rds.static_state->index))
        continue;

      rds.descriptor.set_will_notify_on_start(true);
      rds.descriptor.set_will_notify_on_stop(true);
      backend.producer->service_->RegisterDataSource(rds.descriptor);
      backend.producer->registered_data_sources_.set(rds.static_state->index);
    }
  }
}

void TracingMuxerImpl::SetupTracingSession(
    TracingSessionGlobalID session_id,
    const std::shared_ptr<TraceConfig>& trace_config,
    base::ScopedFile trace_fd) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_CHECK(!trace_fd || trace_config->write_into_file());

  auto* consumer = FindConsumer(session_id);
  if (!consumer)
    return;

  consumer->trace_config_ = trace_config;
  if (trace_fd)
    consumer->trace_fd_ = std::move(trace_fd);

  if (!consumer->connected_)
    return;

  // Only used in the deferred start mode.
  if (trace_config->deferred_start()) {
    consumer->service_->EnableTracing(*trace_config,
                                      std::move(consumer->trace_fd_));
  }
}

void TracingMuxerImpl::StartTracingSession(TracingSessionGlobalID session_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  auto* consumer = FindConsumer(session_id);

  if (!consumer)
    return;

  if (!consumer->trace_config_) {
    PERFETTO_ELOG("Must call Setup(config) first");
    return;
  }

  if (!consumer->connected_) {
    consumer->start_pending_ = true;
    return;
  }

  consumer->start_pending_ = false;
  if (consumer->trace_config_->deferred_start()) {
    consumer->service_->StartTracing();
  } else {
    consumer->service_->EnableTracing(*consumer->trace_config_,
                                      std::move(consumer->trace_fd_));
  }

  // TODO implement support for the deferred-start + fast-triggering case.
}

void TracingMuxerImpl::StopTracingSession(TracingSessionGlobalID session_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto* consumer = FindConsumer(session_id);
  if (!consumer)
    return;

  if (!consumer->trace_config_) {
    PERFETTO_ELOG("Must call Setup(config) and Start() first");
    return;
  }

  if (consumer->start_pending_) {
    // If the session hasn't started yet, wait until it does before stopping.
    consumer->stop_pending_ = true;
    return;
  }

  consumer->stop_pending_ = false;
  if (consumer->stopped_) {
    // If the session was already stopped (e.g., it failed to start), don't try
    // stopping again.
    consumer->NotifyStopComplete();
  } else {
    consumer->service_->DisableTracing();
  }

  consumer->trace_config_.reset();
}

void TracingMuxerImpl::DestroyTracingSession(
    TracingSessionGlobalID session_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (RegisteredBackend& backend : backends_) {
    auto pred = [session_id](const std::unique_ptr<ConsumerImpl>& consumer) {
      return consumer->session_id_ == session_id;
    };
    backend.consumers.erase(std::remove_if(backend.consumers.begin(),
                                           backend.consumers.end(), pred),
                            backend.consumers.end());
  }
}

void TracingMuxerImpl::ReadTracingSessionData(
    TracingSessionGlobalID session_id,
    std::function<void(TracingSession::ReadTraceCallbackArgs)> callback) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  auto* consumer = FindConsumer(session_id);
  if (!consumer)
    return;
  PERFETTO_DCHECK(!consumer->read_trace_callback_);
  consumer->read_trace_callback_ = std::move(callback);
  consumer->service_->ReadBuffers();
}

TracingMuxerImpl::ConsumerImpl* TracingMuxerImpl::FindConsumer(
    TracingSessionGlobalID session_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (RegisteredBackend& backend : backends_) {
    for (auto& consumer : backend.consumers) {
      if (consumer->session_id_ == session_id)
        return consumer.get();
    }
  }
  return nullptr;
}

TracingMuxerImpl::FindDataSourceRes TracingMuxerImpl::FindDataSource(
    TracingBackendId backend_id,
    DataSourceInstanceID instance_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (const auto& rds : data_sources_) {
    DataSourceStaticState* static_state = rds.static_state;
    for (uint32_t i = 0; i < kMaxDataSourceInstances; i++) {
      auto* internal_state = static_state->TryGet(i);
      if (internal_state && internal_state->backend_id == backend_id &&
          internal_state->data_source_instance_id == instance_id) {
        return FindDataSourceRes(static_state, internal_state, i);
      }
    }
  }
  return FindDataSourceRes();
}

// Can be called from any thread.
std::unique_ptr<TraceWriterBase> TracingMuxerImpl::CreateTraceWriter(
    DataSourceState* data_source) {
  ProducerImpl* producer = backends_[data_source->backend_id].producer.get();
  // We choose BufferExhaustedPolicy::kDrop to avoid stalls when all SMB chunks
  // are allocated, ensuring that the app keeps working even when tracing hits
  // its SMB limit. Note that this means we will lose data in such a case
  // (tracked in BufferStats::trace_writer_packet_loss). To reduce this data
  // loss, apps should choose a large enough SMB size.
  return producer->service_->CreateTraceWriter(data_source->buffer_id,
                                               BufferExhaustedPolicy::kDrop);
}

// This is called via the public API Tracing::NewTrace().
// Can be called from any thread.
std::unique_ptr<TracingSession> TracingMuxerImpl::CreateTracingSession(
    BackendType backend_type) {
  TracingSessionGlobalID session_id = ++next_tracing_session_id_;

  // |backend_type| can only specify one backend, not an OR-ed mask.
  PERFETTO_CHECK((backend_type & (backend_type - 1)) == 0);

  // Capturing |this| is fine because the TracingMuxer is a leaky singleton.
  task_runner_->PostTask([this, backend_type, session_id] {
    for (RegisteredBackend& backend : backends_) {
      if (backend_type && backend.type != backend_type)
        continue;

      backend.consumers.emplace_back(
          new ConsumerImpl(this, backend.id, session_id));
      auto& consumer = backend.consumers.back();
      TracingBackend::ConnectConsumerArgs conn_args;
      conn_args.consumer = consumer.get();
      conn_args.task_runner = task_runner_.get();
      consumer->Initialize(backend.backend->ConnectConsumer(conn_args));
      return;
    }
    PERFETTO_ELOG(
        "Cannot create tracing session, no tracing backend ready for type=%d",
        backend_type);
  });

  return std::unique_ptr<TracingSession>(
      new TracingSessionImpl(this, session_id));
}

void TracingMuxerImpl::InitializeInstance(const TracingInitArgs& args) {
  if (instance_)
    PERFETTO_FATAL("Tracing already initialized");
  instance_ = new TracingMuxerImpl(args);
}

TracingMuxer::~TracingMuxer() = default;

static_assert(std::is_same<internal::BufferId, BufferID>::value,
              "public's BufferId and tracing/core's BufferID diverged");

}  // namespace internal
}  // namespace perfetto
