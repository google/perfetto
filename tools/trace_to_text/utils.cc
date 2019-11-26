/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "tools/trace_to_text/utils.h"

#include <inttypes.h>
#include <stdio.h>

#include <memory>
#include <ostream>
#include <set>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"

#include "protos/perfetto/trace/profiling/heap_graph.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_to_text {
namespace {

using Iterator = trace_processor::TraceProcessor::Iterator;

constexpr const char* kQueryUnsymbolized =
    "select spm.name, spm.build_id, spf.rel_pc "
    "from stack_profile_frame spf "
    "join stack_profile_mapping spm "
    "on spf.mapping = spm.id "
    "where spm.build_id != '' and spf.symbol_set_id == 0";

constexpr size_t kCompressionBufferSize = 500 * 1024;

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

using NameAndBuildIdPair = std::pair<std::string, std::string>;

std::map<NameAndBuildIdPair, std::vector<uint64_t>> GetUnsymbolizedFrames(
    trace_processor::TraceProcessor* tp) {
  std::map<std::pair<std::string, std::string>, std::vector<uint64_t>> res;
  Iterator it = tp->ExecuteQuery(kQueryUnsymbolized);
  while (it.Next()) {
    auto name_and_buildid =
        std::make_pair(it.Get(0).string_value, FromHex(it.Get(1).string_value));
    int64_t rel_pc = it.Get(2).long_value;
    res[name_and_buildid].emplace_back(rel_pc);
  }
  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            it.Status().message().c_str());
    return {};
  }
  return res;
}

std::map<std::string, std::set<std::string>> GetHeapGraphClasses(
    trace_processor::TraceProcessor* tp) {
  std::map<std::string, std::set<std::string>> res;
  Iterator it = tp->ExecuteQuery("select type_name from heap_graph_object");
  while (it.Next())
    res.emplace(it.Get(0).string_value, std::set<std::string>());

  PERFETTO_CHECK(it.Status().ok());

  it = tp->ExecuteQuery("select field_name from heap_graph_reference");
  while (it.Next()) {
    std::string field_name = it.Get(0).string_value;
    if (field_name.size() == 0)
      continue;
    size_t n = field_name.rfind(".");
    if (n == std::string::npos || n == field_name.size() - 1) {
      PERFETTO_ELOG("Invalid field name: %s", field_name.c_str());
      continue;
    }
    std::string class_name = field_name;
    class_name.resize(n);
    field_name = field_name.substr(n + 1);
    res[class_name].emplace(field_name);
  }

  PERFETTO_CHECK(it.Status().ok());
  return res;
}

using ::protozero::proto_utils::kMessageLengthFieldSize;
using ::protozero::proto_utils::MakeTagLengthDelimited;
using ::protozero::proto_utils::WriteVarInt;

}  // namespace

void WriteTracePacket(const std::string& str, std::ostream* output) {
  constexpr char kPreamble =
      MakeTagLengthDelimited(protos::pbzero::Trace::kPacketFieldNumber);
  uint8_t length_field[10];
  uint8_t* end = WriteVarInt(str.size(), length_field);
  *output << kPreamble;
  *output << std::string(length_field, end);
  *output << str;
}

void ForEachPacketBlobInTrace(
    std::istream* input,
    const std::function<void(std::unique_ptr<char[]>, size_t)>& f) {
  size_t bytes_processed = 0;
  // The trace stream can be very large. We cannot just pass it in one go to
  // libprotobuf as that will refuse to parse messages > 64MB. However we know
  // that a trace is merely a sequence of TracePackets. Here we just manually
  // tokenize the repeated TracePacket messages and parse them individually
  // using libprotobuf.
  for (uint32_t i = 0;; i++) {
    if ((i & 0x3f) == 0) {
      fprintf(stderr, "Processing trace: %8zu KB%c", bytes_processed / 1024,
              kProgressChar);
      fflush(stderr);
    }
    // A TracePacket consists in one byte stating its field id and type ...
    char preamble;
    input->get(preamble);
    if (!input->good())
      break;
    bytes_processed++;
    PERFETTO_DCHECK(preamble == 0x0a);  // Field ID:1, type:length delimited.

    // ... a varint stating its size ...
    uint32_t field_size = 0;
    uint32_t shift = 0;
    for (;;) {
      char c = 0;
      input->get(c);
      field_size |= static_cast<uint32_t>(c & 0x7f) << shift;
      shift += 7;
      bytes_processed++;
      if (!(c & 0x80))
        break;
    }

    // ... and the actual TracePacket itself.
    std::unique_ptr<char[]> buf(new char[field_size]);
    input->read(buf.get(), static_cast<std::streamsize>(field_size));
    bytes_processed += field_size;

    f(std::move(buf), field_size);
  }
}

std::vector<std::string> GetPerfettoBinaryPath() {
  std::vector<std::string> roots;
  const char* root = getenv("PERFETTO_BINARY_PATH");
  if (root != nullptr) {
    for (base::StringSplitter sp(std::string(root), ':'); sp.Next();)
      roots.emplace_back(sp.cur_token(), sp.cur_token_size());
  }
  return roots;
}

base::Optional<std::string> GetPerfettoProguardMapPath() {
  base::Optional<std::string> proguard_map;
  const char* env = getenv("PERFETTO_PROGUARD_MAP");
  if (env != nullptr)
    proguard_map = env;
  return proguard_map;
}

bool ReadTrace(trace_processor::TraceProcessor* tp, std::istream* input) {
  // 1MB chunk size seems the best tradeoff on a MacBook Pro 2013 - i7 2.8 GHz.
  constexpr size_t kChunkSize = 1024 * 1024;

// Printing the status update on stderr can be a perf bottleneck. On WASM print
// status updates more frequently because it can be slower to parse each chunk.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
  constexpr int kStderrRate = 1;
#else
  constexpr int kStderrRate = 128;
#endif
  uint64_t file_size = 0;

  for (int i = 0;; i++) {
    if (i % kStderrRate == 0) {
      fprintf(stderr, "Loading trace %.2f MB%c", file_size / 1.0e6,
              kProgressChar);
      fflush(stderr);
    }

    std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunkSize]);
    input->read(reinterpret_cast<char*>(buf.get()), kChunkSize);
    if (input->bad()) {
      PERFETTO_ELOG("Failed when reading trace");
      return false;
    }

    auto rsize = input->gcount();
    if (rsize <= 0)
      break;
    file_size += static_cast<uint64_t>(rsize);
    tp->Parse(std::move(buf), static_cast<size_t>(rsize));
  }

  fprintf(stderr, "Loaded trace%c", kProgressChar);
  fflush(stderr);
  return true;
}

void SymbolizeDatabase(trace_processor::TraceProcessor* tp,
                       Symbolizer* symbolizer,
                       std::function<void(const std::string&)> callback) {
  PERFETTO_CHECK(symbolizer);
  auto unsymbolized = GetUnsymbolizedFrames(tp);
  for (auto it = unsymbolized.cbegin(); it != unsymbolized.cend(); ++it) {
    const auto& name_and_buildid = it->first;
    const std::vector<uint64_t>& rel_pcs = it->second;
    auto res = symbolizer->Symbolize(name_and_buildid.first,
                                     name_and_buildid.second, rel_pcs);
    if (res.empty())
      continue;

    protozero::HeapBuffered<perfetto::protos::pbzero::TracePacket> packet;
    auto* module_symbols = packet->set_module_symbols();
    module_symbols->set_path(name_and_buildid.first);
    module_symbols->set_build_id(name_and_buildid.second);
    PERFETTO_DCHECK(res.size() == rel_pcs.size());
    for (size_t i = 0; i < res.size(); ++i) {
      auto* address_symbols = module_symbols->add_address_symbols();
      address_symbols->set_address(rel_pcs[i]);
      for (const SymbolizedFrame& frame : res[i]) {
        auto* line = address_symbols->add_lines();
        line->set_function_name(frame.function_name);
        line->set_source_file_name(frame.file_name);
        line->set_line_number(frame.line);
      }
    }
    callback(packet.SerializeAsString());
  }
}

void DeobfuscateDatabase(
    trace_processor::TraceProcessor* tp,
    const std::map<std::string, profiling::ObfuscatedClass>& mapping,
    std::function<void(const std::string&)> callback) {
  std::map<std::string, std::set<std::string>> classes =
      GetHeapGraphClasses(tp);
  protozero::HeapBuffered<perfetto::protos::pbzero::TracePacket> packet;
  // TODO(fmayer): Add handling for package name and version code here so we
  // can support multiple dumps in the same trace.
  auto* proto_mapping = packet->set_deobfuscation_mapping();
  for (const auto& p : classes) {
    const std::string& obfuscated_class_name = p.first;
    const std::set<std::string>& obfuscated_field_names = p.second;
    auto it = mapping.find(obfuscated_class_name);
    if (it == mapping.end()) {
      // This can happen for non-obfuscated class names. Do not log.
      continue;
    }
    const profiling::ObfuscatedClass& cls = it->second;
    auto* proto_class = proto_mapping->add_obfuscated_classes();
    proto_class->set_obfuscated_name(obfuscated_class_name);
    proto_class->set_deobfuscated_name(cls.deobfuscated_name);
    for (const std::string& obfuscated_field_name : obfuscated_field_names) {
      auto field_it = cls.deobfuscated_fields.find(obfuscated_field_name);
      if (field_it != cls.deobfuscated_fields.end()) {
        auto* proto_member = proto_class->add_obfuscated_members();
        proto_member->set_obfuscated_name(obfuscated_field_name);
        proto_member->set_deobfuscated_name(field_it->second);
      } else {
        PERFETTO_ELOG("%s.%s not found", obfuscated_class_name.c_str(),
                      obfuscated_field_name.c_str());
      }
    }
  }
  callback(packet.SerializeAsString());
}

TraceWriter::TraceWriter(std::ostream* output) : output_(output) {}

TraceWriter::~TraceWriter() = default;

void TraceWriter::Write(std::string s) {
  Write(s.data(), s.size());
}

void TraceWriter::Write(const char* data, size_t sz) {
  output_->write(data, static_cast<std::streamsize>(sz));
}

DeflateTraceWriter::DeflateTraceWriter(std::ostream* output)
    : TraceWriter(output),
      buf_(base::PagedMemory::Allocate(kCompressionBufferSize)),
      start_(static_cast<uint8_t*>(buf_.Get())),
      end_(start_ + buf_.size()) {
  CheckEq(deflateInit(&stream_, 9), Z_OK);
  stream_.next_out = start_;
  stream_.avail_out = static_cast<unsigned int>(end_ - start_);
}

DeflateTraceWriter::~DeflateTraceWriter() {
  while (deflate(&stream_, Z_FINISH) != Z_STREAM_END) {
    Flush();
  }
  CheckEq(deflateEnd(&stream_), Z_OK);
}

void DeflateTraceWriter::Write(const char* data, size_t sz) {
  stream_.next_in = reinterpret_cast<uint8_t*>(const_cast<char*>(data));
  stream_.avail_in = static_cast<unsigned int>(sz);
  while (stream_.avail_in > 0) {
    CheckEq(deflate(&stream_, Z_NO_FLUSH), Z_OK);
    if (stream_.avail_out == 0) {
      Flush();
    }
  }
}

void DeflateTraceWriter::Flush() {
  TraceWriter::Write(reinterpret_cast<char*>(start_),
                     static_cast<size_t>(stream_.next_out - start_));
  stream_.next_out = start_;
  stream_.avail_out = static_cast<unsigned int>(end_ - start_);
}

void DeflateTraceWriter::CheckEq(int actual_code, int expected_code) {
  if (actual_code == expected_code)
    return;
  PERFETTO_FATAL("Expected %d got %d: %s", actual_code, expected_code,
                 stream_.msg);
}

}  // namespace trace_to_text
}  // namespace perfetto
