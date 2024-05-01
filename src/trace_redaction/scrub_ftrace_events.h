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

#ifndef SRC_TRACE_REDACTION_SCRUB_FTRACE_EVENTS_H_
#define SRC_TRACE_REDACTION_SCRUB_FTRACE_EVENTS_H_

#include <string>

#include "perfetto/protozero/field.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

class FtraceEventFilter {
 public:
  virtual ~FtraceEventFilter();

  // Checks if the context contains all neccessary parameters.
  virtual base::Status VerifyContext(const Context& context) const = 0;

  virtual bool KeepEvent(const Context& context,
                         protozero::ConstBytes bytes) const = 0;
};

//  Assumptions:
//    1. This is a hot path (a lot of ftrace packets)
//    2. Allocations are slower than CPU cycles.
//
//  Overview:
//    To limit allocations pbzero protos are used to build a new packet. These
//    protos are append-only, so data is not removed from the packet. Instead,
//    data is optionally added to a new packet.
//
//    To limit allocations, the goal is to add data as large chucks rather than
//    small fragments. To do this, a reactive strategy is used. All operations
//    follow a probe-than-act pattern. Before any action can be taken, the
//    input data must be queries to determine the scope. For example:
//
//        [------A------][---B---][------C------]
//                                [---][-D-][---]
//
//        Assume that A and B don't need any work, they can be appended to the
//        output as two large blocks.
//
//        Block C is different, there is a block D that falls within block C.
//        Block D contains sensitive information and should be dropped. When C
//        is probed, it will come back saying that C needs additional redaction.
class ScrubFtraceEvents : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

  // Add a new filter. T must extend FtraceEventFilter.
  template <typename T>
  void emplace_back() {
    filters_.push_back(std::make_unique<T>());
  }

 private:
  bool KeepEvent(const Context& context, protozero::ConstBytes bytes) const;

  std::vector<std::unique_ptr<FtraceEventFilter>> filters_;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_SCRUB_FTRACE_EVENTS_H_
