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

#ifndef SRC_TRACE_REDACTION_REDACT_FTRACE_EVENTS_H_
#define SRC_TRACE_REDACTION_REDACT_FTRACE_EVENTS_H_

#include <string>

#include "perfetto/protozero/field.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

class FtraceEventFilter {
 public:
  virtual ~FtraceEventFilter();
  virtual bool Includes(const Context& context,
                        protozero::Field event) const = 0;
};

class FtraceEventWriter {
 public:
  virtual ~FtraceEventWriter();
  virtual base::Status WriteTo(
      const Context& context,
      protozero::Field event,
      protos::pbzero::FtraceEventBundle* message) const = 0;
};

class FilterFtracesUsingAllowlist : public FtraceEventFilter {
 public:
  bool Includes(const Context& context, protozero::Field event) const override;
};

class FilterFtraceUsingSuspendResume : public FtraceEventFilter {
 public:
  bool Includes(const Context& context, protozero::Field event) const override;
};

class WriteFtracesPassthrough : public FtraceEventWriter {
  base::Status WriteTo(
      const Context& context,
      protozero::Field event,
      protos::pbzero::FtraceEventBundle* message) const override;
};

// Filters ftrace events and modifies remaining events before writing them to
// the packet. Only one filter and/or writer can be assigned to provide finer
// grain control.
class RedactFtraceEvents : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

  template <typename Filter>
  void emplace_filter() {
    filter_ = std::make_unique<Filter>();
  }

  template <typename Writer>
  void emplace_writer() {
    writer_ = std::make_unique<Writer>();
  }

 private:
  // Checks if there is at least one ftrace event.
  bool HasFtraceEvent(const std::string& bytes) const;

  base::Status OnFtraceEvents(const Context& context,
                              protozero::ConstBytes bytes,
                              protos::pbzero::FtraceEventBundle* message) const;

  std::unique_ptr<FtraceEventFilter> filter_;
  std::unique_ptr<FtraceEventWriter> writer_;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_REDACT_FTRACE_EVENTS_H_
