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

#ifndef SRC_TRACE_REDACTION_REDACT_FTRACE_EVENT_H_
#define SRC_TRACE_REDACTION_REDACT_FTRACE_EVENT_H_

#include <cstdint>

#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"

namespace perfetto::trace_redaction {

// Redaction and "scrubing" are two different operations. Scrubbing removes the
// whole event. Redaction removes fields from with-in the event, but keeps the
// event in the trace.
class FtraceEventRedaction {
 public:
  explicit FtraceEventRedaction(uint32_t field_id) : field_id_(field_id) {}

  virtual ~FtraceEventRedaction();

  uint32_t field_id() const { return field_id_; }

  // Write a new version of the event to the message.
  virtual base::Status Redact(
      const Context& context,
      const protos::pbzero::FtraceEvent::Decoder& event,
      protozero::ConstBytes bytes,
      protos::pbzero::FtraceEvent* event_message) const = 0;

 private:
  uint32_t field_id_;
};

class RedactFtraceEvent : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

  // Add a new redaction. T must extend FtraceEventRedaction.
  template <typename T>
  void emplace_back() {
    redactions_.push_back(std::make_unique<T>());
  }

 private:
  void RedactPacket(const Context& context,
                    protozero::ConstBytes bytes,
                    protos::pbzero::TracePacket* message) const;

  void RedactEvents(const Context& context,
                    protozero::ConstBytes bytes,
                    protos::pbzero::FtraceEventBundle* message) const;

  void RedactEvent(const Context& context,
                   protozero::ConstBytes bytes,
                   protos::pbzero::FtraceEvent* message) const;

  const FtraceEventRedaction* FindRedactionFor(uint32_t i) const;

  // Each redactions supplies its own id. This list will be small. So
  // iterating over the list checking the reported ids should be good enough.
  // Ids must not collide.
  std::vector<std::unique_ptr<FtraceEventRedaction>> redactions_;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_REDACT_FTRACE_EVENT_H_
