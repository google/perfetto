/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_PROFILING_MEMORY_HEAPPROFD_PRODUCER_H_
#define SRC_PROFILING_MEMORY_HEAPPROFD_PRODUCER_H_

#include <functional>
#include <map>

#include "perfetto/base/optional.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/unix_socket.h"
#include "perfetto/base/unix_task_runner.h"

#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/tracing_service.h"

#include "src/profiling/memory/bookkeeping.h"
#include "src/profiling/memory/proc_utils.h"
#include "src/profiling/memory/system_property.h"
#include "src/profiling/memory/unwinding.h"

namespace perfetto {
namespace profiling {

struct Process {
  pid_t pid;
  std::string cmdline;
};

class LogHistogram {
 public:
  static const uint64_t kMaxBucket;
  static constexpr size_t kBuckets = 20;

  void Add(uint64_t value) { values_[GetBucket(value)]++; }
  std::vector<std::pair<uint64_t, uint64_t>> GetData();

 private:
  size_t GetBucket(uint64_t value);

  std::array<uint64_t, kBuckets> values_ = {};
};

// TODO(rsavitski): central daemon can do less work if it knows that the global
// operating mode is fork-based, as it then will not be interacting with the
// clients. This can be implemented as an additional mode here.
enum class HeapprofdMode { kCentral, kChild };

// Heap profiling producer. Can be instantiated in two modes, central and
// child (also referred to as fork mode).
//
// The central mode producer is instantiated by the system heapprofd daemon. Its
// primary responsibility is activating profiling (via system properties and
// signals) in targets identified by profiling configs. On debug platform
// builds, the central producer can also handle the out-of-process unwinding &
// writing of the profiles for all client processes.
//
// An alternative model is where the central heapprofd triggers the profiling in
// the target process, but the latter fork-execs a private heapprofd binary to
// handle unwinding only for that process. The forked heapprofd instantiates
// this producer in the "child" mode. In this scenario, the profiled process
// never talks to the system daemon.
//
// TODO(fmayer||rsavitski): cover interesting invariants/structure of the
// implementation (e.g. number of data sources in child mode), including
// threading structure.
class HeapprofdProducer : public Producer, public UnwindingWorker::Delegate {
 public:
  friend class SocketDelegate;

  // TODO(fmayer): Split into two delegates for the listening socket in kCentral
  // and for the per-client sockets to make this easier to understand?
  // Alternatively, find a better name for this.
  class SocketDelegate : public base::UnixSocket::EventListener {
   public:
    SocketDelegate(HeapprofdProducer* producer) : producer_(producer) {}

    void OnDisconnect(base::UnixSocket* self) override;
    void OnNewIncomingConnection(
        base::UnixSocket* self,
        std::unique_ptr<base::UnixSocket> new_connection) override;
    void OnDataAvailable(base::UnixSocket* self) override;

   private:
    HeapprofdProducer* producer_;
  };

  HeapprofdProducer(HeapprofdMode mode, base::TaskRunner* task_runner);
  ~HeapprofdProducer() override;

  // Producer Impl:
  void OnConnect() override;
  void OnDisconnect() override;
  void SetupDataSource(DataSourceInstanceID, const DataSourceConfig&) override;
  void StartDataSource(DataSourceInstanceID, const DataSourceConfig&) override;
  void StopDataSource(DataSourceInstanceID) override;
  void OnTracingSetup() override;
  void Flush(FlushRequestID,
             const DataSourceInstanceID* data_source_ids,
             size_t num_data_sources) override;

  // TODO(fmayer): Refactor once/if we have generic reconnect logic.
  void ConnectWithRetries(const char* socket_name);
  void DumpAll();

  // UnwindingWorker::Delegate impl:
  void PostAllocRecord(AllocRecord) override;
  void PostFreeRecord(FreeRecord) override;
  void PostSocketDisconnected(DataSourceInstanceID,
                              pid_t,
                              SharedRingBuffer::Stats) override;

  void HandleAllocRecord(AllocRecord);
  void HandleFreeRecord(FreeRecord);
  void HandleSocketDisconnected(DataSourceInstanceID,
                                pid_t,
                                SharedRingBuffer::Stats);

  // Valid only if mode_ == kChild.
  void SetTargetProcess(pid_t target_pid,
                        std::string target_cmdline,
                        base::ScopedFile inherited_socket);
  // Valid only if mode_ == kChild. Kicks off a periodic check that the child
  // heapprofd is actively working on a data source (which should correspond to
  // the target process). The first check is delayed to let the freshly spawned
  // producer get the data sources from the tracing service (i.e. traced).
  void ScheduleActiveDataSourceWatchdog();

  // Exposed for testing.
  void SetProducerEndpoint(
      std::unique_ptr<TracingService::ProducerEndpoint> endpoint);

 private:
  void HandleClientConnection(std::unique_ptr<base::UnixSocket> new_connection,
                              Process process);

  enum State {
    kNotStarted = 0,
    kNotConnected,
    kConnecting,
    kConnected,
  };
  void ConnectService();
  void Restart();
  void ResetConnectionBackoff();
  void IncreaseConnectionBackoff();

  State state_ = kNotStarted;
  uint32_t connection_backoff_ms_ = 0;
  const char* producer_sock_name_ = nullptr;

  const HeapprofdMode mode_;

  void FinishDataSourceFlush(FlushRequestID flush_id);
  bool Dump(DataSourceInstanceID id,
            FlushRequestID flush_id,
            bool has_flush_id);
  void DoContinuousDump(DataSourceInstanceID id, uint32_t dump_interval);
  UnwindingWorker& UnwinderForPID(pid_t);

  // functionality specific to mode_ == kCentral
  std::unique_ptr<base::UnixSocket> MakeListeningSocket();

  // functionality specific to mode_ == kChild
  void TerminateProcess(int exit_status);
  void ActiveDataSourceWatchdogCheck();
  // Adopts the (connected) sockets inherited from the target process, invoking
  // the on-connection callback.
  void AdoptTargetProcessSocket();

  struct ProcessState {
    ProcessState(GlobalCallstackTrie* callsites) : heap_tracker(callsites) {}
    bool disconnected = false;
    bool buffer_overran = false;
    bool buffer_corrupted = false;

    uint64_t heap_samples = 0;
    uint64_t map_reparses = 0;
    uint64_t unwinding_errors = 0;

    uint64_t total_unwinding_time_us = 0;
    LogHistogram unwinding_time_us;
    HeapTracker heap_tracker;
  };

  struct DataSource {
    DataSourceInstanceID id;
    std::unique_ptr<TraceWriter> trace_writer;
    HeapprofdConfig config;
    ClientConfiguration client_configuration;
    std::vector<SystemProperties::Handle> properties;
    std::set<pid_t> signaled_pids;
    std::set<pid_t> rejected_pids;
    std::map<pid_t, ProcessState> process_states;
    std::vector<std::string> normalized_cmdlines;
    uint64_t next_index_ = 0;
  };

  struct PendingProcess {
    std::unique_ptr<base::UnixSocket> sock;
    DataSourceInstanceID data_source_instance_id;
    SharedRingBuffer shmem;
  };

  std::map<pid_t, PendingProcess> pending_processes_;

  bool IsPidProfiled(pid_t);
  DataSource* GetDataSourceForProcess(const Process& proc);
  void RecordOtherSourcesAsRejected(DataSource* active_ds, const Process& proc);

  std::map<DataSourceInstanceID, DataSource> data_sources_;
  std::map<FlushRequestID, size_t> flushes_in_progress_;

  // Task runner is owned by the main thread.
  base::TaskRunner* const task_runner_;
  std::unique_ptr<TracingService::ProducerEndpoint> endpoint_;

  GlobalCallstackTrie callsites_;
  std::vector<UnwindingWorker> unwinding_workers_;

  // state specific to mode_ == kCentral
  std::unique_ptr<base::UnixSocket> listening_socket_;
  SystemProperties properties_;

  // state specific to mode_ == kChild
  Process target_process_{base::kInvalidPid, ""};
  // This is a valid FD between SetTargetProcess and AdoptTargetProcessSocket
  // only.
  base::ScopedFile inherited_fd_;

  SocketDelegate socket_delegate_;

  base::WeakPtrFactory<HeapprofdProducer> weak_factory_;  // Keep last.
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_HEAPPROFD_PRODUCER_H_
