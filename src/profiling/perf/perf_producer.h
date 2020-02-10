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

#ifndef SRC_PROFILING_PERF_PERF_PRODUCER_H_
#define SRC_PROFILING_PERF_PERF_PRODUCER_H_

#include <deque>
#include <map>
#include <queue>

#include <unistd.h>

#include <unwindstack/Regs.h>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "src/profiling/common/callstack_trie.h"
#include "src/profiling/common/interning_output.h"
#include "src/profiling/common/unwind_support.h"
#include "src/profiling/perf/event_config.h"
#include "src/profiling/perf/event_reader.h"
#include "src/profiling/perf/proc_descriptors.h"

namespace perfetto {
namespace profiling {

// TODO(b/144281346): work in progress. Do not use.
class PerfProducer : public Producer, public ProcDescriptorDelegate {
 public:
  PerfProducer(ProcDescriptorGetter* proc_fd_getter,
               base::TaskRunner* task_runner);
  ~PerfProducer() override = default;

  PerfProducer(const PerfProducer&) = delete;
  PerfProducer& operator=(const PerfProducer&) = delete;
  PerfProducer(PerfProducer&&) = delete;
  PerfProducer& operator=(PerfProducer&&) = delete;

  void ConnectWithRetries(const char* socket_name);

  // Producer impl:
  void OnConnect() override;
  void OnDisconnect() override;
  void OnTracingSetup() override {}
  void SetupDataSource(DataSourceInstanceID, const DataSourceConfig&) override;
  void StartDataSource(DataSourceInstanceID instance_id,
                       const DataSourceConfig& config) override;
  void StopDataSource(DataSourceInstanceID instance_id) override;
  void Flush(FlushRequestID flush_id,
             const DataSourceInstanceID* data_source_ids,
             size_t num_data_sources) override;

  // TODO(rsavitski): clear ds->interning_output, then re-emit fixed internings.
  void ClearIncrementalState(const DataSourceInstanceID* /*data_source_ids*/,
                             size_t /*num_data_sources*/) override {}

  // ProcDescriptorDelegate impl:
  void OnProcDescriptors(pid_t pid,
                         base::ScopedFile maps_fd,
                         base::ScopedFile mem_fd) override;

 private:
  // State of the connection to tracing service (traced).
  enum State {
    kNotStarted = 0,
    kNotConnected,
    kConnecting,
    kConnected,
  };

  struct DataSource {
    DataSource(std::unique_ptr<TraceWriter> _trace_writer,
               EventReader _event_reader)
        : trace_writer(std::move(_trace_writer)),
          event_reader(std::move(_event_reader)) {}

    std::unique_ptr<TraceWriter> trace_writer;

    // TODO(rsavitski): event reader per kernel buffer.
    EventReader event_reader;

    // TODO(rsavitski): under a single-threaded model, directly shared between
    // the reader and the "unwinder". If/when lifting unwinding into a separate
    // thread(s), the FDs will become owned by the unwinder, but the tracking
    // will need to be done by both sides (frontend needs to know whether to
    // resolve the pid, and the unwinder needs to know whether the fd is
    // ready/poisoned).
    // TODO(rsavitski): find a more descriptive name.
    struct ProcDescriptors {
      enum class Status { kInitial, kResolving, kResolved, kSkip };

      Status status = Status::kInitial;
      UnwindingMetadata unwind_state{/*maps_fd=*/base::ScopedFile{},
                                     /*mem_fd=*/base::ScopedFile{}};
    };
    std::map<pid_t, ProcDescriptors> proc_fds;  // keyed by pid

    // Tracks the incremental state for interned entries.
    InterningOutputTracker interning_output;

    bool reader_stopping = false;
    bool unwind_stopping = false;
  };

  // Entry in an unwinding queue. Either a sample that requires unwinding, or a
  // tombstoned entry (valid == false).
  struct UnwindEntry {
    UnwindEntry(ParsedSample _sample)
        : valid(true), sample(std::move(_sample)) {}

    bool valid = false;
    ParsedSample sample;
  };

  // Fully processed sample that is ready for output.
  struct CompletedSample {
    // move-only
    CompletedSample() = default;
    CompletedSample(const CompletedSample&) = delete;
    CompletedSample& operator=(const CompletedSample&) = delete;
    CompletedSample(CompletedSample&&) = default;
    CompletedSample& operator=(CompletedSample&&) = default;

    uint32_t cpu = 0;
    pid_t pid = 0;
    pid_t tid = 0;
    uint64_t timestamp = 0;
    std::vector<FrameData> frames;
    bool unwind_error = false;
  };

  void ConnectService();
  void Restart();
  void ResetConnectionBackoff();
  void IncreaseConnectionBackoff();

  void TickDataSourceRead(DataSourceInstanceID ds_id);

  void PostDescriptorLookupTimeout(DataSourceInstanceID ds_id,
                                   pid_t pid,
                                   uint32_t timeout_ms);
  void DescriptorLookupTimeout(DataSourceInstanceID ds_id, pid_t pid);

  void TickDataSourceUnwind(DataSourceInstanceID ds_id);
  // Returns true if we should keep processing the queue (i.e. we should
  // continue the unwinder ticks).
  bool ProcessUnwindQueue(DataSourceInstanceID ds_id,
                          std::deque<UnwindEntry>* input_queue,
                          DataSource* ds_ptr);
  CompletedSample UnwindSample(ParsedSample sample,
                               DataSource::ProcDescriptors* process_state);

  void PostEmitSample(DataSourceInstanceID ds_id, CompletedSample sample);
  void EmitSample(DataSourceInstanceID ds_id, CompletedSample sample);

  // Starts the shutdown of the given data source instance, starting with the
  // reader frontend.
  void InitiateReaderStop(DataSource* ds);
  // TODO(rsavitski): under a dedicated unwind thread, this becomes a PostTask
  // for the instance id.
  void InitiateUnwindStop(DataSource* ds);
  // Destroys the state belonging to this instance, and notifies the tracing
  // service of the stop.
  void FinishDataSourceStop(DataSourceInstanceID ds_id);

  // Task runner owned by the main thread.
  base::TaskRunner* const task_runner_;
  State state_ = kNotStarted;
  const char* producer_socket_name_ = nullptr;
  uint32_t connection_backoff_ms_ = 0;

  // Valid and stable for the lifetime of this class.
  ProcDescriptorGetter* const proc_fd_getter_;

  // Owns shared memory, must outlive trace writing.
  std::unique_ptr<TracingService::ProducerEndpoint> endpoint_;

  // Interns callstacks across all data sources.
  // TODO(rsavitski): for long profiling sessions, consider purging trie when it
  // grows too large (at the moment purged only when no sources are active).
  // TODO(rsavitski): interning sequences are monotonic for the lifetime of the
  // daemon. Consider resetting them at safe points - possible when no sources
  // are active, and tricky otherwise. In the latter case, it'll require
  // emitting incremental sequence invalidation packets on all relevant
  // sequences.
  GlobalCallstackTrie callstack_trie_;

  std::map<DataSourceInstanceID, DataSource> data_sources_;
  std::map<DataSourceInstanceID, std::deque<UnwindEntry>> unwind_queues_;

  base::WeakPtrFactory<PerfProducer> weak_factory_;  // keep last
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_PERF_PRODUCER_H_
