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

#include "tools/trace_to_text/pprof_builder.h"

#include <cxxabi.h>
#include <inttypes.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "tools/trace_to_text/profile_visitor.h"
#include "tools/trace_to_text/symbolizer.h"
#include "tools/trace_to_text/trace_symbol_table.h"
#include "tools/trace_to_text/utils.h"

#include "perfetto/base/logging.h"

#include "protos/perfetto/trace/profiling/profile_common.pb.h"
#include "protos/perfetto/trace/profiling/profile_packet.pb.h"
#include "protos/perfetto/trace/trace.pb.h"
#include "protos/perfetto/trace/trace_packet.pb.h"
#include "protos/third_party/pprof/profile.pb.h"

namespace perfetto {
namespace trace_to_text {

namespace {

void MaybeDemangle(std::string* name) {
  int ignored;
  char* data = abi::__cxa_demangle(name->c_str(), nullptr, nullptr, &ignored);
  if (data) {
    *name = data;
    free(data);
  }
}

using ::perfetto::protos::Callstack;
using ::perfetto::protos::Frame;
using ::perfetto::protos::InternedString;
using ::perfetto::protos::Mapping;
using ::perfetto::protos::ProfilePacket;
using ::perfetto::protos::InternedData;

using GLine = ::perfetto::third_party::perftools::profiles::Line;
using GMapping = ::perfetto::third_party::perftools::profiles::Mapping;
using GLocation = ::perfetto::third_party::perftools::profiles::Location;
using GProfile = ::perfetto::third_party::perftools::profiles::Profile;
using GValueType = ::perfetto::third_party::perftools::profiles::ValueType;
using GFunction = ::perfetto::third_party::perftools::profiles::Function;
using GSample = ::perfetto::third_party::perftools::profiles::Sample;

std::string ToHex(const std::string& build_id) {
  std::string hex_build_id(2 * build_id.size() + 1, ' ');
  for (size_t i = 0; i < build_id.size(); ++i)
    snprintf(&(hex_build_id[2 * i]), 3, "%02hhx", build_id[i]);
  // Remove the trailing nullbyte.
  hex_build_id.resize(2 * build_id.size());
  return hex_build_id;
}

enum Strings : int64_t {
  kEmpty = 0,
  kObjects,
  kAllocObjects,
  kCount,
  kSpace,
  kAllocSpace,
  kBytes,
  kIdleSpace,
  kMaxSpace,
};

class GProfileWriter : public ProfileVisitor {
 public:
  GProfileWriter(TraceSymbolTable* symbol_table) : symbol_table_(symbol_table) {
    GValueType* value_type = profile_.add_sample_type();
    value_type->set_type(kMaxSpace);
    value_type->set_unit(kBytes);

    value_type = profile_.add_sample_type();
    value_type->set_type(kObjects);
    value_type->set_unit(kCount);

    value_type = profile_.add_sample_type();
    value_type->set_type(kAllocObjects);
    value_type->set_unit(kCount);

    value_type = profile_.add_sample_type();
    value_type->set_type(kIdleSpace);
    value_type->set_unit(kBytes);

    value_type = profile_.add_sample_type();
    value_type->set_type(kAllocSpace);
    value_type->set_unit(kBytes);

    // The last value is the default one selected.
    value_type = profile_.add_sample_type();
    value_type->set_type(kSpace);
    value_type->set_unit(kBytes);
  }

  int64_t InternInGProfile(const std::string& str) {
    decltype(string_table_)::iterator it;
    std::tie(it, std::ignore) =
        string_table_.emplace(str, string_table_.size());
    return static_cast<int64_t>(it->second);
  }

  bool AddInternedString(const InternedString& interned_string) override {
    string_lookup_.emplace(interned_string.iid(), interned_string.str());
    return true;
  }

  bool AddCallstack(const Callstack& callstack) override {
    std::vector<uint64_t> frame_ids(
        static_cast<size_t>(callstack.frame_ids().size()));
    std::reverse_copy(callstack.frame_ids().cbegin(),
                      callstack.frame_ids().cend(), frame_ids.begin());
    callstack_lookup_.emplace(callstack.iid(), std::move(frame_ids));
    return true;
  }

  bool AddMapping(const Mapping& mapping) override {
    mapping_base_.emplace(mapping.iid(), mapping.start() - mapping.load_bias());
    GMapping* gmapping = profile_.add_mapping();
    gmapping->set_id(mapping.iid());
    gmapping->set_memory_start(mapping.start());
    gmapping->set_memory_limit(mapping.end());
    gmapping->set_file_offset(mapping.exact_offset());
    std::string filename;
    for (uint64_t str_id : mapping.path_string_ids()) {
      auto it = string_lookup_.find(str_id);
      if (it == string_lookup_.end()) {
        PERFETTO_ELOG("Mapping %" PRIu64
                      " referring to invalid string_id %" PRIu64 ".",
                      static_cast<uint64_t>(mapping.iid()), str_id);
        return false;
      }

      filename += "/" + it->second;
    }

    gmapping->set_filename(InternInGProfile(filename));

    auto str_it = string_lookup_.find(mapping.build_id());
    if (str_it != string_lookup_.end()) {
      const std::string& build_id = str_it->second;
      gmapping->set_build_id(InternInGProfile(ToHex(build_id)));
    }
    return true;
  }

  bool AddFrame(const Frame& frame) override {
    auto it = mapping_base_.find(frame.mapping_id());
    if (it == mapping_base_.end()) {
      PERFETTO_ELOG("Frame referring to invalid mapping ID %" PRIu64,
                    static_cast<uint64_t>(frame.mapping_id()));
      return false;
    }
    uint64_t mapping_base = it->second;

    GLocation* glocation = profile_.add_location();
    glocation->set_id(frame.iid());
    glocation->set_mapping_id(frame.mapping_id());
    glocation->set_address(frame.rel_pc() + mapping_base);

    const std::vector<SymbolizedFrame>* symbolized_frames =
        symbol_table_->Get(frame.iid());
    std::vector<SymbolizedFrame> frames;
    if (symbolized_frames == nullptr) {
      // Write out whatever was in the profile initially.
      auto str_it = string_lookup_.find(frame.function_name_id());
      if (str_it == string_lookup_.end()) {
        PERFETTO_ELOG("Function referring to invalid string id %" PRIu64,
                      static_cast<uint64_t>(frame.function_name_id()));
        return false;
      }
      frames.emplace_back(SymbolizedFrame{str_it->second, "", 0});
    } else {
      frames = *symbolized_frames;
    }

    for (const SymbolizedFrame& sym_frame : frames) {
      GLine* gline = glocation->add_line();
      uint64_t function_id = ++max_function_id_;
      gline->set_function_id(function_id);
      gline->set_line(sym_frame.line);
      std::string function_name = sym_frame.function_name;
      // This assumes both the device that captured the trace and the host
      // machine use the same mangling scheme. This is a reasonable
      // assumption as the Itanium ABI is the de-facto standard for mangling.
      MaybeDemangle(&function_name);
      GFunction* gfunction = profile_.add_function();
      gfunction->set_id(function_id);
      gfunction->set_name(InternInGProfile(function_name));
      gfunction->set_filename(InternInGProfile(sym_frame.file_name));
    }
    return true;
  }

  bool AddProfiledFrameSymbols(const protos::ProfiledFrameSymbols&) override {
    return true;
  }

  bool Finalize() {
    // We keep the interning table as string -> uint64_t for fast and easy
    // lookup. When dumping, we need to turn it into a uint64_t -> string
    // table so we get it sorted by key order.
    std::map<uint64_t, std::string> inverted_string_table;
    for (const auto& p : string_table_)
      inverted_string_table[p.second] = p.first;
    for (const auto& p : inverted_string_table)
      profile_.add_string_table(p.second);
    return true;
  }

  bool WriteProfileForProcess(
      uint64_t pid,
      const std::vector<const ProfilePacket::ProcessHeapSamples*>& proc_samples,
      std::string* serialized) {
    GProfile cur_profile = profile_;
    for (const ProfilePacket::ProcessHeapSamples* samples : proc_samples) {
      if (samples->rejected_concurrent()) {
        PERFETTO_ELOG("WARNING: The profile for %" PRIu64
                      " was rejected due to a concurrent profile.",
                      pid);
      }
      if (samples->buffer_overran()) {
        PERFETTO_ELOG("WARNING: The profile for %" PRIu64
                      " ended early due to a buffer overrun.",
                      pid);
      }
      if (samples->buffer_corrupted()) {
        PERFETTO_ELOG("WARNING: The profile for %" PRIu64
                      " ended early due to a buffer corruption."
                      " THIS IS ALWAYS A BUG IN HEAPPROFD OR"
                      " CLIENT MEMORY CORRUPTION.",
                      pid);
      }

      for (const ProfilePacket::HeapSample& sample : samples->samples()) {
        GSample* gsample = cur_profile.add_sample();
        auto it = callstack_lookup_.find(sample.callstack_id());
        if (it == callstack_lookup_.end()) {
          PERFETTO_ELOG("Callstack referring to invalid callstack id %" PRIu64,
                        static_cast<uint64_t>(sample.callstack_id()));
          return false;
        }
        for (uint64_t frame_id : it->second)
          gsample->add_location_id(frame_id);
        gsample->add_value(static_cast<int64_t>(sample.self_max()));
        gsample->add_value(
            static_cast<int64_t>(sample.alloc_count() - sample.free_count()));
        gsample->add_value(static_cast<int64_t>(sample.alloc_count()));
        gsample->add_value(static_cast<int64_t>(sample.self_idle()));
        gsample->add_value(static_cast<int64_t>(sample.self_allocated()));
        gsample->add_value(static_cast<int64_t>(sample.self_allocated() -
                                                sample.self_freed()));
      }
    }
    *serialized = cur_profile.SerializeAsString();
    return true;
  }

 private:
  TraceSymbolTable* symbol_table_;
  GProfile profile_;

  uint64_t max_function_id_ = 0;

  std::map<uint64_t, uint64_t> mapping_base_;
  std::set<uint64_t> functions_to_dump_;
  std::map<uint64_t, const std::vector<uint64_t>> callstack_lookup_;
  std::map<uint64_t, std::string> string_lookup_;
  std::map<std::string, uint64_t> string_table_{
      {"", kEmpty},
      {"objects", kObjects},
      {"alloc_objects", kAllocObjects},
      {"count", kCount},
      {"space", kSpace},
      {"alloc_space", kAllocSpace},
      {"bytes", kBytes},
      {"idle_space", kIdleSpace},
      {"max_space", kMaxSpace}};
};

bool DumpProfilePacket(const std::vector<ProfilePacket>& packet_fragments,
                       const std::vector<InternedData>& interned_data,
                       std::vector<SerializedProfile>* output,
                       Symbolizer* symbolizer) {
  TraceSymbolTable symbol_table(symbolizer);
  if (!symbol_table.Visit(packet_fragments, interned_data))
    return false;
  if (!symbol_table.Finalize())
    return false;

  GProfileWriter writer(&symbol_table);
  if (!writer.Visit(packet_fragments, interned_data))
    return false;

  if (!writer.Finalize())
    return false;

  std::map<uint64_t, std::vector<const ProfilePacket::ProcessHeapSamples*>>
      heap_samples;
  for (const ProfilePacket& packet : packet_fragments) {
    for (const ProfilePacket::ProcessHeapSamples& samples :
         packet.process_dumps()) {
      heap_samples[samples.pid()].emplace_back(&samples);
    }
  }
  for (const auto& p : heap_samples) {
    std::string serialized;
    if (!writer.WriteProfileForProcess(p.first, p.second, &serialized))
      return false;
    output->emplace_back(SerializedProfile{p.first, std::move(serialized)});
  }
  return true;
}

}  // namespace

bool TraceToPprof(std::istream* input,
                  std::vector<SerializedProfile>* output,
                  Symbolizer* symbolizer) {
  return VisitCompletePacket(
      input, [output, symbolizer](
                 uint32_t, const std::vector<ProfilePacket>& packet_fragments,
                 const std::vector<InternedData>& interned_data) {
        return DumpProfilePacket(packet_fragments, interned_data, output,
                                 symbolizer);
      });
}

bool TraceToPprof(std::istream* input, std::vector<SerializedProfile>* output) {
  return TraceToPprof(input, output, nullptr);
}

}  // namespace trace_to_text
}  // namespace perfetto
