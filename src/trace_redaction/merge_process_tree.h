/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_REDACTION_MERGE_PROCESS_TREE_H_
#define SRC_TRACE_REDACTION_MERGE_PROCESS_TREE_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

class ProcessTreeProcess {
 public:
  std::vector<std::string> cmdline;
  bool is_kthread;
  int32_t pid;
  int32_t ppid;
  int32_t uid;
};

class ProcessTreeThread {
 public:
  int32_t tid;
  int32_t tgid;
  std::string name;
};

// Collects and merges thread/process information from the trace and outputs a
// single trace packet containing all the unique thread/process data.
//
// This is useful because threads/process data is typically duplicated across
// multiple packets in a trace after Transform phase of redaction. This
// primitive consolidates that data into a single packet to eliminate
// redundancy.
class MergeProcessTree : public AugmentReducePrimitive {
 public:
  ~MergeProcessTree() override = default;

  base::Status Collect(const protos::pbzero::TracePacket::Decoder& packet,
                       const Context* context) override;

  std::string Augment(const Context& context) override;

  base::Status Reduce(const Context* context,
                      std::string* packet) const override;

 private:
  std::unordered_map<int32_t, ProcessTreeProcess> processes_by_pid_;
  std::unordered_map<int32_t, ProcessTreeThread> threads_by_tid_;
  uint64_t timestamp_ = 0;
  int32_t trusted_uid_ = 0;
  bool collected_global_packet_fields_ = false;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_MERGE_PROCESS_TREE_H_
