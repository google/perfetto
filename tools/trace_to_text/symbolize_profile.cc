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

#include "tools/trace_to_text/symbolize_profile.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <elf.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "perfetto/protozero/proto_utils.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/utils.h"

#include "tools/trace_to_text/local_symbolizer.h"
#include "tools/trace_to_text/profile_visitor.h"
#include "tools/trace_to_text/symbolizer.h"
#include "tools/trace_to_text/utils.h"

#include "perfetto/trace/profiling/profile_common.pb.h"
#include "perfetto/trace/profiling/profile_packet.pb.h"
#include "perfetto/trace/trace.pbzero.h"
#include "perfetto/trace/trace_packet.pb.h"

#include "perfetto/trace/interned_data/interned_data.pb.h"

namespace perfetto {
namespace trace_to_text {
namespace {

using ::protozero::proto_utils::kMessageLengthFieldSize;
using ::protozero::proto_utils::MakeTagLengthDelimited;
using ::protozero::proto_utils::WriteVarInt;

using ::perfetto::protos::Callstack;
using ::perfetto::protos::Frame;
using ::perfetto::protos::InternedData;
using ::perfetto::protos::InternedString;
using ::perfetto::protos::Mapping;
using ::perfetto::protos::ProfiledFrameSymbols;
using ::perfetto::protos::ProfilePacket;

void WriteTracePacket(const std::string& str, std::ostream* output) {
  constexpr char kPreamble =
      MakeTagLengthDelimited(protos::pbzero::Trace::kPacketFieldNumber);
  uint8_t length_field[10];
  uint8_t* end = WriteVarInt(str.size(), length_field);
  *output << kPreamble;
  *output << std::string(length_field, end);
  *output << str;
}

class TraceSymbolTable : public ProfileVisitor {
 public:
  TraceSymbolTable(Symbolizer* symbolizer) : symbolizer_(symbolizer) {}

  bool AddCallstack(const Callstack&) override { return true; }

  bool AddInternedString(const InternedString& string) override {
    interned_strings_.emplace(string.iid(), string.str());
    max_string_intern_id_ =
        std::max<uint64_t>(string.iid(), max_string_intern_id_);
    return true;
  }

  bool AddMapping(const Mapping& mapping) override {
    mappings_.emplace(mapping.iid(), ResolveMapping(mapping));
    return true;
  }

  bool AddFrame(const Frame& frame) override {
    auto it = mappings_.find(frame.mapping_id());
    if (it == mappings_.end()) {
      PERFETTO_ELOG("Invalid mapping.");
      return false;
    }
    const ResolvedMapping& mapping = it->second;
    auto result = symbolizer_->Symbolize(mapping.mapping_name, mapping.build_id,
                                         {frame.rel_pc()});
    if (!result.empty())
      symbols_for_frame_[frame.iid()] = std::move(result[0]);
    return true;
  }

  void WriteResult(std::ostream* output, uint32_t seq_id) {
    std::map<std::string, uint64_t> new_interned_strings;
    protos::TracePacket intern_packet;
    intern_packet.set_trusted_packet_sequence_id(seq_id);
    protos::InternedData* interned_data = intern_packet.mutable_interned_data();
    for (const auto& p : symbols_for_frame_) {
      const std::vector<SymbolizedFrame>& frames = p.second;
      for (const SymbolizedFrame& frame : frames) {
        uint64_t& function_name_id = new_interned_strings[frame.function_name];
        if (function_name_id == 0) {
          function_name_id = ++max_string_intern_id_;
          protos::InternedString* str = interned_data->add_function_names();
          str->set_iid(function_name_id);
          str->set_str(
              reinterpret_cast<const uint8_t*>(frame.function_name.c_str()),
              frame.function_name.size());
        }

        uint64_t& source_file_id = new_interned_strings[frame.file_name];
        if (source_file_id == 0) {
          source_file_id = ++max_string_intern_id_;
          protos::InternedString* str = interned_data->add_source_paths();
          str->set_iid(source_file_id);
          str->set_str(
              reinterpret_cast<const uint8_t*>(frame.file_name.c_str()),
              frame.file_name.size());
        }
      }
    }

    WriteTracePacket(intern_packet.SerializeAsString(), output);

    for (const auto& p : symbols_for_frame_) {
      uint64_t frame_iid = p.first;
      const std::vector<SymbolizedFrame>& frames = p.second;
      protos::TracePacket packet;
      packet.set_trusted_packet_sequence_id(seq_id);
      protos::ProfiledFrameSymbols* sym =
          packet.mutable_profiled_frame_symbols();
      sym->set_frame_iid(static_cast<int64_t>(frame_iid));
      for (const SymbolizedFrame& frame : frames) {
        // TODO(fmayer): Sort out types here. Make the function_name_id and
        // file_name_id uint64, this requires a Chrome change as well.
        sym->add_function_name_id(
            static_cast<int64_t>(new_interned_strings[frame.function_name]));
        sym->add_line_number(static_cast<int32_t>(frame.line));
        sym->add_file_name_id(
            static_cast<int64_t>(new_interned_strings[frame.file_name]));
      }

      WriteTracePacket(packet.SerializeAsString(), output);
    }
  }

 private:
  struct ResolvedMapping {
    std::string mapping_name;
    std::string build_id;
  };

  std::string ResolveString(uint64_t iid) {
    auto it = interned_strings_.find(iid);
    if (it == interned_strings_.end())
      return {};
    return it->second;
  }

  ResolvedMapping ResolveMapping(const Mapping& mapping) {
    std::string path;
    for (uint64_t iid : mapping.path_string_ids()) {
      path += "/";
      path += ResolveString(iid);
    }
    return {std::move(path), ResolveString(mapping.build_id())};
  }

  Symbolizer* symbolizer_;

  std::map<uint64_t, std::string> interned_strings_;
  std::map<uint64_t, ResolvedMapping> mappings_;

  std::map<std::string, uint64_t> intern_table_;
  uint64_t max_string_intern_id_ = 0;

  std::map<uint64_t /* frame_id */, std::vector<SymbolizedFrame>>
      symbols_for_frame_;
};

}  // namespace

// Ingest profile, and emit a symbolization table for each sequence. This can
// be prepended to the profile to attach the symbol information.
int SymbolizeProfile(std::istream* input, std::ostream* output) {
  LocalSymbolizer local_symbolizer(GetPerfettoBinaryPath());

  return VisitCompletePacket(
      input,
      [&output, &local_symbolizer](
          uint32_t seq_id, const std::vector<ProfilePacket>& packet_fragments,
          const std::vector<InternedData>& interned_data) {
        TraceSymbolTable symbolizer(&local_symbolizer);
        if (!symbolizer.Visit(packet_fragments, interned_data))
          return false;
        symbolizer.WriteResult(output, seq_id);
        return true;
      });
}

}  // namespace trace_to_text
}  // namespace perfetto
