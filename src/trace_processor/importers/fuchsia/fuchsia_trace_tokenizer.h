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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FUCHSIA_FUCHSIA_TRACE_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FUCHSIA_FUCHSIA_TRACE_TOKENIZER_H_

#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_utils.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/task_state.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// The Fuchsia trace format is documented at
// https://fuchsia.googlesource.com/fuchsia/+/HEAD/docs/development/tracing/trace-format/README.md
class FuchsiaTraceTokenizer : public ChunkedTraceReader {
 public:
  explicit FuchsiaTraceTokenizer(TraceProcessorContext*);
  ~FuchsiaTraceTokenizer() override;

  // ChunkedTraceReader implementation
  util::Status Parse(TraceBlobView) override;
  void NotifyEndOfFile() override;

 private:
  struct ProviderInfo {
    std::string name;

    std::unordered_map<uint64_t, StringId> string_table;
    std::unordered_map<uint64_t, FuchsiaThreadInfo> thread_table;

    uint64_t ticks_per_second = 1000000000;
  };

  // Tracks the state for updating sched slice and thread state tables.
  struct Thread {
    explicit Thread(uint64_t tid) : info{0, tid} {}

    FuchsiaThreadInfo info;
    int64_t last_ts{0};
    base::Optional<tables::SchedSliceTable::RowNumber> last_slice_row;
    base::Optional<tables::ThreadStateTable::RowNumber> last_state_row;
  };

  // Allocates or returns an existing Thread instance for the given tid.
  Thread& GetThread(uint64_t tid) {
    auto search = threads_.find(tid);
    if (search != threads_.end()) {
      return search->second;
    }
    auto result = threads_.emplace(tid, tid);
    return result.first->second;
  }

  void ParseRecord(TraceBlobView);
  void RegisterProvider(uint32_t, std::string);
  StringId IdForOutgoingThreadState(uint32_t state);

  TraceProcessorContext* const context_;
  std::vector<uint8_t> leftover_bytes_;

  // Proto reader creates state that the blobs it emits reference, so the
  // proto_reader needs to live for as long as the tokenizer.
  ProtoTraceReader proto_reader_;
  std::vector<uint8_t> proto_trace_data_;

  std::unordered_map<uint32_t, std::unique_ptr<ProviderInfo>> providers_;
  ProviderInfo* current_provider_;

  // Interned string ids for the relevant thread states.
  StringId running_string_id_;
  StringId runnable_string_id_;
  StringId preempted_string_id_;
  StringId blocked_string_id_;
  StringId suspended_string_id_;
  StringId exit_dying_string_id_;
  StringId exit_dead_string_id_;

  // Map from tid to Thread.
  std::unordered_map<uint64_t, Thread> threads_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FUCHSIA_FUCHSIA_TRACE_TOKENIZER_H_
