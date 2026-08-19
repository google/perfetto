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

// Collects unique thread/process information from the trace and saves it
// into Context::merged_process_tree.
class CollectProcessTrees : public CollectPrimitive {
 public:
  ~CollectProcessTrees() override = default;

  base::Status Collect(const protos::pbzero::TracePacket::Decoder& packet,
                       Context* context) const override;
};

// Removes process tree packets from the trace (since they will be replaced
// by the merged process tree in the augment phase).
class ReduceProcessTrees : public TransformPrimitive {
 public:
  ~ReduceProcessTrees() override = default;

  base::Status Transform(const Context& context,
                         std::string* packet) const override;
};

// Outputs a single trace packet containing all unique thread/process data
// collected in Context::merged_process_tree.
class AugmentProcessTrees : public AugmentPrimitive {
 public:
  ~AugmentProcessTrees() override = default;

  std::string Augment(const Context& context) override;

 private:
  bool emitted_ = false;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_MERGE_PROCESS_TREE_H_
