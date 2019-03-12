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

// TODO(rsavitski): central daemon can do less work if it knows that the global
// operating mode is fork-based, as it then will not be interacting with the
// clients. This can be implemented as an additional mode here.
enum class HeapprofdMode { kCentral, kChild };

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

  // TODO(fmayer): Delete once we have generic reconnect logic.
  void ConnectWithRetries(const char* socket_name);
  void DumpAll();

  // UnwindingWorker::Delegate impl:
  void PostAllocRecord(AllocRecord) override;
  void PostFreeRecord(FreeRecord) override;
  void PostSocketDisconnected(DataSourceInstanceID, pid_t) override;

  void HandleAllocRecord(AllocRecord);
  void HandleFreeRecord(FreeRecord);
  void HandleSocketDisconnected(DataSourceInstanceID, pid_t);

  // Valid only if mode_ == kChild.
  void SetTargetProcess(pid_t target_pid,
                        std::string target_cmdline,
                        base::ScopedFile inherited_socket);

 private:
  void HandleClientConnection(std::unique_ptr<base::UnixSocket> new_connection,
                              Process process);

  // TODO(fmayer): Delete once we have generic reconnect logic.
  enum State {
    kNotStarted = 0,
    kNotConnected,
    kConnecting,
    kConnected,
  };
  void Connect();
  void Restart();
  void ResetConnectionBackoff();
  void IncreaseConnectionBackoff();

  // TODO(fmayer): Delete once we have generic reconnect logic.
  State state_ = kNotStarted;
  uint32_t connection_backoff_ms_ = 0;
  const char* producer_sock_name_ = nullptr;

  const HeapprofdMode mode_;

  std::vector<std::thread> MakeUnwindingThreads(size_t n);
  std::vector<UnwindingWorker> MakeUnwindingWorkers(size_t n);

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
  bool SourceMatchesTarget(const HeapprofdConfig& cfg);

  // Valid only if mode_ == kChild. Adopts the (connected) sockets inherited
  // from the target process, invoking the on-connection callback.
  void AdoptTargetProcessSocket();

  struct DataSource {
    DataSourceInstanceID id;
    std::unique_ptr<TraceWriter> trace_writer;
    HeapprofdConfig config;
    ClientConfiguration client_configuration;
    std::vector<SystemProperties::Handle> properties;
    std::map<pid_t, HeapTracker> heap_trackers;
  };

  struct PendingProcess {
    std::unique_ptr<base::UnixSocket> sock;
    DataSourceInstanceID data_source_instance_id;
    SharedRingBuffer shmem;
  };

  std::map<pid_t, PendingProcess> pending_processes_;

  DataSource* GetDataSourceForProcess(const Process& proc);

  std::map<DataSourceInstanceID, DataSource> data_sources_;
  std::map<FlushRequestID, size_t> flushes_in_progress_;

  // These two are borrowed from the caller.
  base::TaskRunner* const task_runner_;
  std::unique_ptr<TracingService::ProducerEndpoint> endpoint_;

  GlobalCallstackTrie callsites_;
  // Sequence number for ProfilePackets, so the consumer can assert that none
  // of them were dropped.
  uint64_t next_index_ = 0;

  // These are not fields in UnwinderThread as the task runner is not movable
  // and that makes UnwinderThread very unwieldy objects (e.g. we cannot
  // emplace_back into a vector as that requires movability.)
  std::vector<base::UnixTaskRunner> unwinding_task_runners_;
  std::vector<std::thread> unwinding_threads_;  // Only for ownership.
  std::vector<UnwindingWorker> unwinding_workers_;

  // state specific to mode_ == kCentral
  std::unique_ptr<base::UnixSocket> listening_socket_;
  SystemProperties properties_;

  // state specific to mode_ == kChild
  pid_t target_pid_ = base::kInvalidPid;
  std::string target_cmdline_;
  // This is a valid FD between SetTargetProcess and UseTargetProcessSocket
  // only.
  base::ScopedFile inherited_fd_;

  SocketDelegate socket_delegate_;

  base::WeakPtrFactory<HeapprofdProducer> weak_factory_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_HEAPPROFD_PRODUCER_H_
