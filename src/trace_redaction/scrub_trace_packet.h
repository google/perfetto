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

#ifndef SRC_TRACE_REDACTION_SCRUB_TRACE_PACKET_H_
#define SRC_TRACE_REDACTION_SCRUB_TRACE_PACKET_H_

#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

class TracePacketFilter {
 public:
  virtual ~TracePacketFilter();

  // Checks if the context contains all neccessary parameters.
  virtual base::Status VerifyContext(const Context& context) const;

  // Checks if the field should be pass onto the new packet. Checks are a
  // logical AND, so all filters must return true.
  virtual bool KeepField(const Context& context,
                         const protozero::Field& field) const = 0;
};

class ScrubTracePacket : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

  template <typename T>
  void emplace_back() {
    filters_.emplace_back(new T());
  }

 private:
  bool KeepEvent(const Context& context, const protozero::Field& field) const;

  std::vector<std::unique_ptr<TracePacketFilter>> filters_;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_SCRUB_TRACE_PACKET_H_
