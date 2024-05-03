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
#include <memory>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto::trace_redaction {

// Redaction and "scrubing" are two different operations. Scrubbing removes the
// whole event. Redaction removes fields from with-in the event, but keeps the
// event in the trace.
class FtraceEventRedaction {
 public:
  virtual ~FtraceEventRedaction();

  // Write at most one field from `event` to `event_message`. This relies on the
  // honor system; other redactions may be registered on other values.
  //
  //  - event: effectively "protos::pbzero::FtraceEvent::Decoder"
  virtual base::Status Redact(
      const Context& context,
      const protos::pbzero::FtraceEventBundle::Decoder& bundle,
      protozero::ProtoDecoder& event,
      protos::pbzero::FtraceEvent* event_message) const = 0;
};

class RedactFtraceEvent : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

  // Add a new redaction. T must extend FtraceEventRedaction. This relies on the
  // honor system; no more than one redaction can be mapped to a field.
  template <uint32_t field_id, class T>
  void emplace_back() {
    redactions_.Insert(field_id, std::make_unique<T>());
  }

 private:
  void RedactEvents(const Context& context,
                    protozero::Field bundle,
                    protos::pbzero::FtraceEventBundle* message) const;

  void RedactEvent(const Context& context,
                   const protos::pbzero::FtraceEventBundle::Decoder& bundle,
                   protozero::Field event,
                   protos::pbzero::FtraceEvent* message) const;

  base::FlatHashMap<uint32_t, std::unique_ptr<FtraceEventRedaction>>
      redactions_;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_REDACT_FTRACE_EVENT_H_
