/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_REDACTION_MODIFY_PROCESS_TREES_H_
#define SRC_TRACE_REDACTION_MODIFY_PROCESS_TREES_H_

#include <string>

#include "perfetto/base/status.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ps/process_tree.pbzero.h"

namespace perfetto::trace_redaction {

// Walk through process trees, calling process and thread handlers to add new
// process and threads messages to the process tree. If the default handler is
// not replaced, the thread/process will be added to the parent.
class ModifyProcessTree : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

 protected:
  // Verifies that the context contains required values. No-op by default.
  virtual base::Status VerifyContext(const Context& context) const;

  // Modifies a process before adding it back to the process tree. Appends the
  // field to the process tree without modification by default.
  virtual void TransformProcess(
      const Context& context,
      const protozero::Field& timestamp,
      const protozero::Field& process,
      protos::pbzero::ProcessTree* process_tree) const;

  // Modifies a thread before adding it back to the process tree. Appends the
  // field to the process tree without modification by default.
  virtual void TransformThread(
      const Context& context,
      const protozero::Field& timestamp,
      const protozero::Field& thread,
      protos::pbzero::ProcessTree* process_trees) const;

  // TODO(vaage): Add a handler that is called the process tree is populated so
  // that fields can be added to process tree (e.g. creating new threads -
  // needed for thread merging).

 private:
  void TransformProcessTree(const Context& context,
                            const protozero::Field& timestamp,
                            const protozero::Field& process_tree,
                            protos::pbzero::ProcessTree* message) const;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_MODIFY_PROCESS_TREES_H_
