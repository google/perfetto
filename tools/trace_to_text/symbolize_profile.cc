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
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/utils.h"

#include "tools/trace_to_text/local_symbolizer.h"
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

using ::perfetto::protos::Frame;
using ::perfetto::protos::InternedData;
using ::perfetto::protos::InternedString;
using ::perfetto::protos::Mapping;
using ::perfetto::protos::ProfilePacket;

std::vector<std::string> GetRootsForEnv() {
  std::vector<std::string> roots;
  const char* root = getenv("PERFETTO_BINARY_PATH");
  if (root != nullptr) {
    for (base::StringSplitter sp(std::string(root), ':'); sp.Next();)
      roots.emplace_back(sp.cur_token(), sp.cur_token_size());
  }
  return roots;
}

class SymbolizedTraceRewriter {
 public:
  SymbolizedTraceRewriter(std::unique_ptr<Symbolizer> symbolizer)
      : symbolizer_(std::move(symbolizer)) {}

  void AddInternedString(const InternedString& string) {
    interned_strings_.emplace(string.iid(), string.str());
  }

  void AddMapping(const Mapping& mapping) {
    mappings_.emplace(mapping.iid(), ResolveMapping(mapping));
  }

  void SymbolizeFrame(Frame* frame) {
    auto it = mappings_.find(frame->mapping_id());
    if (it == mappings_.end()) {
      PERFETTO_ELOG("Invalid mapping.");
      return;
    }
    const ResolvedMapping& mapping = it->second;
    auto result = symbolizer_->Symbolize(mapping.mapping_name, mapping.build_id,
                                         frame->rel_pc());
    if (!result.empty()) {
      // TODO(fmayer): Better support for inline functions.
      const SymbolizedFrame& symf = result[0];
      if (symf.function_name != "??") {
        uint64_t& id = intern_table_[symf.function_name];
        if (!id)
          id = --intern_id_;
        frame->set_function_name_id(id);
      }
    }
  }

  const std::map<std::string, uint64_t>& intern_table() const {
    return intern_table_;
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

  std::unique_ptr<Symbolizer> symbolizer_;

  std::map<uint64_t, std::string> interned_strings_;
  std::map<uint64_t, ResolvedMapping> mappings_;

  std::map<std::string, uint64_t> intern_table_;
  // Use high IDs for the newly interned strings to avoid clashing with
  // other interned strings. The other solution is to read the trace twice
  // in order to find out the maximum used interned ID. This means that we
  // cannot operate on stdin anymore.
  uint64_t intern_id_ = std::numeric_limits<uint64_t>::max();
};

void WriteTracePacket(const std::string& str, std::ostream* output) {
  constexpr char kPreamble =
      MakeTagLengthDelimited(protos::pbzero::Trace::kPacketFieldNumber);
  uint8_t length_field[10];
  uint8_t* end = WriteVarInt(str.size(), length_field);
  *output << kPreamble;
  *output << std::string(length_field, end);
  *output << str;
}

}  // namespace

int SymbolizeProfile(std::istream* input, std::ostream* output) {
  SymbolizedTraceRewriter symbolizer(
      std::unique_ptr<Symbolizer>(new LocalSymbolizer(GetRootsForEnv())));

  ForEachPacketInTrace(input, [&output,
                               &symbolizer](protos::TracePacket packet) {
    protos::ProfilePacket* profile_packet = nullptr;
    if (packet.has_profile_packet()) {
      profile_packet = packet.mutable_profile_packet();
    }
    InternedData* data = nullptr;
    if (packet.has_interned_data())
      data = packet.mutable_interned_data();
    if (profile_packet) {
      for (const InternedString& interned_string : profile_packet->strings())
        symbolizer.AddInternedString(interned_string);
    }
    if (data) {
      for (const InternedString& interned_string : data->build_ids())
        symbolizer.AddInternedString(interned_string);
      for (const InternedString& interned_string : data->mapping_paths())
        symbolizer.AddInternedString(interned_string);
      for (const InternedString& interned_string : data->function_names())
        symbolizer.AddInternedString(interned_string);
    }
    if (profile_packet) {
      for (const Mapping& mapping : profile_packet->mappings())
        symbolizer.AddMapping(mapping);
    }
    if (data) {
      for (const Mapping& mapping : data->mappings())
        symbolizer.AddMapping(mapping);
    }
    if (profile_packet) {
      for (Frame& frame : *profile_packet->mutable_frames())
        symbolizer.SymbolizeFrame(&frame);
    }
    if (data) {
      for (Frame& frame : *data->mutable_frames())
        symbolizer.SymbolizeFrame(&frame);
    }

    // As we will write the newly interned strings after, we need to set
    // continued for the last ProfilePacket.
    if (profile_packet)
      profile_packet->set_continued(true);
    WriteTracePacket(packet.SerializeAsString(), output);
  });

  // We have to emit a ProfilePacket with continued = false to terminate the
  // sequence of related ProfilePackets.
  protos::TracePacket packet;
  const auto& intern_table = symbolizer.intern_table();
  if (!intern_table.empty()) {
    InternedData* data = packet.mutable_interned_data();
    for (const auto& p : intern_table) {
      InternedString* str = data->add_function_names();
      str->set_iid(p.second);
      str->set_str(p.first);
    }
  }
  packet.mutable_profile_packet();
  WriteTracePacket(packet.SerializeAsString(), output);
  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
