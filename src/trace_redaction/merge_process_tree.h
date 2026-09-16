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

using ProcessTreeProcess = Context::ProcessTreeProcess;
using ProcessTreeThread = Context::ProcessTreeThread;

// During redaction multiple process trees with redundant information are
// generated due to merge of all other processes when merging every other
// process and thread into a synthetic process called, so we end up with
// multiple process_tree packets with the same information. A canonical example
// of the issue looks like the following:
//
// process_tree {
//   collection_end_timestamp: 1210106257304
//   processes {
//     uid: 1000
//     ppid: 1
//     pid: 4194305
//     cmdline: "Other-Processes"
//   }
//   threads {
//     tgid: 4194305
//     tid: 4194306
//     name: "cpu-4194306"
//   }
//   ... (the rest of the threads, one for each CPU)
// }
// process_tree { <-- Duplicated packet
//   collection_end_timestamp: 1210202165914
//   processes {
//     uid: 1000
//     ppid: 1
//     pid: 4194305
//     cmdline: "Other-Processes"
//   }
//   threads {
//     tgid: 4194305
//     tid: 4194306
//     name: "cpu-4194306"
//   }
//   ... (the rest of the threads, one for each CPU)
// }
//
// The goal of the primitives in this file is to deduplicate the processtree
// packets so that only a single process tree packet is emitted containing all
// the threads and processes.

// Collects unique thread/process information from the trace
class CollectProcessTrees : public CollectPrimitive {
 public:
  ~CollectProcessTrees() override = default;

  base::Status Collect(const protos::pbzero::TracePacket::Decoder& packet,
                       Context* context) const override;
};

// Removes all existing process tree packets from the trace (since they will be
// replaced by the merged process tree in the augment phase).
class ReduceProcessTrees : public TransformPrimitive {
 public:
  ~ReduceProcessTrees() override = default;

  base::Status Transform(const Context& context,
                         std::string* packet) const override;
};

// Inserts a single trace packet containing all unique thread/process data
class AugmentProcessTrees : public AugmentPrimitive {
 public:
  ~AugmentProcessTrees() override = default;

  base::Status Augment(const Context& context, std::string* packet) override;

 private:
  bool emitted_ = false;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_MERGE_PROCESS_TREE_H_
