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

#ifndef TOOLS_TRACE_TO_TEXT_SYMBOLIZE_PROFILE_H_
#define TOOLS_TRACE_TO_TEXT_SYMBOLIZE_PROFILE_H_

#include <iostream>

#include "tools/trace_to_text/profile_visitor.h"
#include "tools/trace_to_text/symbolizer.h"

namespace perfetto {
namespace trace_to_text {

class TraceSymbolTable : public ProfileVisitor {
 public:
  TraceSymbolTable(Symbolizer* symbolizer) : symbolizer_(symbolizer) {}
  bool AddCallstack(const protos::Callstack&) override { return true; }
  bool AddInternedString(const protos::InternedString& string) override;
  bool AddFrame(const protos::Frame& frame) override;
  bool AddMapping(const protos::Mapping& mapping) override;
  void WriteResult(std::ostream* output, uint32_t seq_id);

 private:
  struct ResolvedMapping {
    std::string mapping_name;
    std::string build_id;
  };

  std::string ResolveString(uint64_t iid);
  ResolvedMapping ResolveMapping(const protos::Mapping& mapping);

  Symbolizer* symbolizer_;

  std::map<uint64_t, std::string> interned_strings_;
  std::map<uint64_t, ResolvedMapping> mappings_;

  std::map<std::string, uint64_t> intern_table_;
  uint64_t max_string_intern_id_ = 0;

  std::map<uint64_t /* frame_id */, std::vector<SymbolizedFrame>>
      symbols_for_frame_;
};
int SymbolizeProfile(std::istream* input, std::ostream* output);

}  // namespace trace_to_text
}  // namespace perfetto

#endif  // TOOLS_TRACE_TO_TEXT_SYMBOLIZE_PROFILE_H_
