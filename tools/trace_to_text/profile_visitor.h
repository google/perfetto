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

#ifndef TOOLS_TRACE_TO_TEXT_PROFILE_VISITOR_H_
#define TOOLS_TRACE_TO_TEXT_PROFILE_VISITOR_H_

#include <vector>

#include "perfetto/base/logging.h"

#include "tools/trace_to_text/utils.h"

#include "protos/perfetto/trace/interned_data/interned_data.pb.h"
#include "protos/perfetto/trace/profiling/profile_common.pb.h"
#include "protos/perfetto/trace/profiling/profile_packet.pb.h"
#include "protos/perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace trace_to_text {

struct SequencedBundle {
  std::vector<protos::InternedData> interned_data;
  std::vector<protos::ProfiledFrameSymbols> symbols;
};

class ProfileVisitor {
 public:
  bool Visit(const std::vector<protos::ProfilePacket>&, const SequencedBundle&);
  virtual bool AddInternedString(
      const protos::InternedString& interned_string) = 0;
  virtual bool AddCallstack(const protos::Callstack& callstack) = 0;
  virtual bool AddMapping(const protos::Mapping& mapping) = 0;
  virtual bool AddFrame(const protos::Frame& frame) = 0;
  virtual bool AddProfiledFrameSymbols(
      const protos::ProfiledFrameSymbols& symbol) = 0;
  virtual ~ProfileVisitor();
};

bool VisitCompletePacket(
    std::istream* input,
    const std::function<bool(uint32_t,
                             const std::vector<protos::ProfilePacket>&,
                             const SequencedBundle&)>& fn);

}  // namespace trace_to_text
}  // namespace perfetto

#endif  // TOOLS_TRACE_TO_TEXT_PROFILE_VISITOR_H_
