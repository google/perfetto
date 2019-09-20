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

#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/profiling/symbolizer.h"

#ifndef PERFETTO_NOLOCALSYMBOLIZE
#include "tools/trace_to_text/local_symbolizer.h"  // nogncheck
#endif

#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "tools/trace_to_text/utils.h"

namespace perfetto {
namespace trace_to_text {
namespace {

using ::protozero::proto_utils::kMessageLengthFieldSize;
using ::protozero::proto_utils::MakeTagLengthDelimited;
using ::protozero::proto_utils::WriteVarInt;

void WriteTracePacket(const std::string& str, std::ostream* output) {
  constexpr char kPreamble =
      MakeTagLengthDelimited(protos::pbzero::Trace::kPacketFieldNumber);
  uint8_t length_field[10];
  uint8_t* end = WriteVarInt(str.size(), length_field);
  *output << kPreamble;
  *output << std::string(length_field, end);
  *output << str;
}

using Iterator = trace_processor::TraceProcessor::Iterator;

constexpr const char* kQueryUnsymbolized =
    "select spm.name, spm.build_id, spf.rel_pc "
    "from stack_profile_frame spf "
    "join stack_profile_mapping spm "
    "on spf.mapping = spm.id "
    "where spm.build_id != '' and spf.symbol_set_id == 0";

std::string FromHex(const char* str, size_t size) {
  if (size % 2) {
    PERFETTO_DFATAL_OR_ELOG("Failed to parse hex %s", str);
    return "";
  }
  std::string result(size / 2, '\0');
  for (size_t i = 0; i < size; i += 2) {
    char hex_byte[3];
    hex_byte[0] = str[i];
    hex_byte[1] = str[i + 1];
    hex_byte[2] = '\0';
    char* end;
    long int byte = strtol(hex_byte, &end, 16);
    if (*end != '\0') {
      PERFETTO_DFATAL_OR_ELOG("Failed to parse hex %s", str);
      return "";
    }
    result[i / 2] = static_cast<char>(byte);
  }
  return result;
}

std::string FromHex(const std::string& str) {
  return FromHex(str.c_str(), str.size());
}

std::map<std::pair<std::string, std::string>, std::vector<uint64_t>>
GetUnsymbolizedFrames(trace_processor::TraceProcessor* tp) {
  std::map<std::pair<std::string, std::string>, std::vector<uint64_t>> res;
  for (Iterator it = tp->ExecuteQuery(kQueryUnsymbolized); it.Next();)
    res[std::make_pair(it.Get(0).string_value, FromHex(it.Get(1).string_value))]
        .emplace_back(it.Get(2).long_value);

  return res;
}

}  // namespace

void SymbolizeDatabase(
    trace_processor::TraceProcessor* tp,
    Symbolizer* symbolizer,
    std::function<void(const perfetto::protos::TracePacket&)> callback) {
  PERFETTO_CHECK(symbolizer);
  auto unsymbolized = GetUnsymbolizedFrames(tp);
  for (auto it = unsymbolized.cbegin(); it != unsymbolized.cend(); ++it) {
    const auto& name_and_buildid = it->first;
    const std::vector<uint64_t>& rel_pcs = it->second;
    auto res = symbolizer->Symbolize(name_and_buildid.first,
                                     name_and_buildid.second, rel_pcs);
    if (res.empty())
      continue;

    perfetto::protos::TracePacket packet;
    perfetto::protos::ModuleSymbols* module_symbols =
        packet.mutable_module_symbols();
    module_symbols->set_path(name_and_buildid.first);
    module_symbols->set_build_id(name_and_buildid.second);
    PERFETTO_DCHECK(res.size() == rel_pcs.size());
    for (size_t i = 0; i < res.size(); ++i) {
      auto* address_symbols = module_symbols->add_address_symbols();
      address_symbols->set_address(rel_pcs[0]);
      for (const SymbolizedFrame& frame : res[i]) {
        auto* line = address_symbols->add_lines();
        line->set_function_name(frame.function_name);
        line->set_source_file_name(frame.file_name);
        line->set_line_number(frame.line);
      }
    }
    callback(packet);
  }
}

// Ingest profile, and emit a symbolization table for each sequence. This can
// be prepended to the profile to attach the symbol information.
int SymbolizeProfile(std::istream* input, std::ostream* output) {
  std::unique_ptr<Symbolizer> symbolizer;
  auto binary_path = GetPerfettoBinaryPath();
  if (!binary_path.empty()) {
#ifndef PERFETTO_NOLOCALSYMBOLIZE
    symbolizer.reset(new LocalSymbolizer(GetPerfettoBinaryPath()));
#else
    PERFETTO_FATAL("This build does not support local symbolization.");
#endif
  }

  if (!symbolizer)
    PERFETTO_FATAL("No symbolizer selected");
  trace_processor::Config config;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);

  if (!ReadTrace(tp.get(), input))
    PERFETTO_FATAL("Failed to read trace.");

  SymbolizeDatabase(tp.get(), symbolizer.get(),
                    [output](const perfetto::protos::TracePacket& packet) {
                      WriteTracePacket(packet.SerializeAsString(), output);
                    });
  return true;
}

}  // namespace trace_to_text
}  // namespace perfetto
