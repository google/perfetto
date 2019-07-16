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

#include "tools/trace_to_text/utils.h"

#include "perfetto/base/logging.h"

#include "perfetto/trace/profiling/profile_common.pb.h"
#include "perfetto/trace/profiling/profile_packet.pb.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

#include "third_party/pprof/profile.pb.h"

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

using GLine = ::perftools::profiles::Line;
using GMapping = ::perftools::profiles::Mapping;
using GLocation = ::perftools::profiles::Location;
using GProfile = ::perftools::profiles::Profile;
using GValueType = ::perftools::profiles::ValueType;
using GFunction = ::perftools::profiles::Function;
using GSample = ::perftools::profiles::Sample;

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

class GProfileWriter {
 public:
  GProfileWriter() {
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

  void AddInternedString(const InternedString& interned_string) {
    string_lookup_.emplace(interned_string.iid(), interned_string.str());
  }

  void AddCallstack(const Callstack& callstack) {
    std::vector<uint64_t> frame_ids(
        static_cast<size_t>(callstack.frame_ids().size()));
    std::reverse_copy(callstack.frame_ids().cbegin(),
                      callstack.frame_ids().cend(), frame_ids.begin());
    callstack_lookup_.emplace(callstack.iid(), std::move(frame_ids));
  }

  bool AddMapping(const Mapping& mapping) {
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

    decltype(string_table_)::iterator it;
    std::tie(it, std::ignore) =
        string_table_.emplace(filename, string_table_.size());
    gmapping->set_filename(static_cast<int64_t>(it->second));

    auto str_it = string_lookup_.find(mapping.build_id());
    if (str_it != string_lookup_.end()) {
      const std::string& build_id = str_it->second;
      std::tie(it, std::ignore) =
          string_table_.emplace(ToHex(build_id), string_table_.size());
      gmapping->set_build_id(static_cast<int64_t>(it->second));
    }
    return true;
  }

  bool AddFrame(const Frame& frame) {
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
    GLine* gline = glocation->add_line();
    gline->set_function_id(frame.function_name_id());
    functions_to_dump_.emplace(frame.function_name_id());
    return true;
  }

  bool Finalize() {
    for (uint64_t function_name_id : functions_to_dump_) {
      auto str_it = string_lookup_.find(function_name_id);
      if (str_it == string_lookup_.end()) {
        PERFETTO_ELOG("Function referring to invalid string id %" PRIu64,
                      function_name_id);
        return false;
      }
      decltype(string_table_)::iterator it;
      std::string function_name = str_it->second;
      // This assumes both the device that captured the trace and the host
      // machine use the same mangling scheme. This is a reasonable
      // assumption as the Itanium ABI is the de-facto standard for mangling.
      MaybeDemangle(&function_name);
      std::tie(it, std::ignore) =
          string_table_.emplace(std::move(function_name), string_table_.size());
      GFunction* gfunction = profile_.add_function();
      gfunction->set_id(function_name_id);
      gfunction->set_name(static_cast<int64_t>(it->second));
    }

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
  GProfile profile_;

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

bool MakeWriter(const std::vector<ProfilePacket>& packet_fragments,
                const std::vector<InternedData>& interned_data,
                GProfileWriter* writer) {
  // A profile packet can be split into multiple fragments. We need to iterate
  // over all of them to reconstruct the original packet.
  for (const ProfilePacket& packet : packet_fragments) {
    for (const InternedString& interned_string : packet.strings())
      writer->AddInternedString(interned_string);
  }
  for (const InternedData& data : interned_data) {
    for (const InternedString& interned_string : data.build_ids())
      writer->AddInternedString(interned_string);
    for (const InternedString& interned_string : data.mapping_paths())
      writer->AddInternedString(interned_string);
    for (const InternedString& interned_string : data.function_names())
      writer->AddInternedString(interned_string);
  }

  for (const ProfilePacket& packet : packet_fragments) {
    for (const Callstack& callstack : packet.callstacks())
      writer->AddCallstack(callstack);
  }
  for (const InternedData& data : interned_data) {
    for (const Callstack& callstack : data.callstacks())
      writer->AddCallstack(callstack);
  }

  for (const ProfilePacket& packet : packet_fragments) {
    for (const Mapping& mapping : packet.mappings())
      writer->AddMapping(mapping);
  }
  for (const InternedData& data : interned_data) {
    for (const Mapping& callstack : data.mappings()) {
      if (!writer->AddMapping(callstack))
        return false;
    }
  }

  for (const ProfilePacket& packet : packet_fragments) {
    for (const Frame& frame : packet.frames()) {
      if (!writer->AddFrame(frame))
        return false;
    }
  }
  for (const InternedData& data : interned_data) {
    for (const Frame& frame : data.frames()) {
      if (!writer->AddFrame(frame))
        return false;
    }
  }
  return writer->Finalize();
}

bool DumpProfilePacket(const std::vector<ProfilePacket>& packet_fragments,
                       const std::vector<InternedData>& interned_data,
                       std::vector<SerializedProfile>* output) {
  GProfileWriter writer;
  if (!MakeWriter(packet_fragments, interned_data, &writer))
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

bool TraceToPprof(std::istream* input, std::vector<SerializedProfile>* output) {
  std::map<uint32_t, std::vector<ProfilePacket>> rolling_profile_packets_by_seq;
  std::map<uint32_t, std::vector<InternedData>> rolling_interned_data_by_seq;
  bool success = true;
  ForEachPacketInTrace(input, [&rolling_profile_packets_by_seq,
                               &rolling_interned_data_by_seq, &output,
                               &success](const protos::TracePacket& packet) {
    uint32_t seq_id = packet.trusted_packet_sequence_id();
    if (packet.has_interned_data())
      rolling_interned_data_by_seq[seq_id].emplace_back(packet.interned_data());

    if (!packet.has_profile_packet())
      return;

    rolling_profile_packets_by_seq[seq_id].emplace_back(
        packet.profile_packet());

    const std::vector<InternedData>& rolling_interned_data =
        rolling_interned_data_by_seq[seq_id];
    const std::vector<ProfilePacket>& rolling_profile_packets =
        rolling_profile_packets_by_seq[seq_id];

    if (!packet.profile_packet().continued()) {
      for (size_t i = 1; i < rolling_profile_packets.size(); ++i) {
        // Ensure we are not missing a chunk.
        if (rolling_profile_packets[i - 1].index() + 1 !=
            rolling_profile_packets[i].index()) {
          success = false;
          return;
        }
      }
      if (!DumpProfilePacket(rolling_profile_packets, rolling_interned_data,
                             output)) {
        success = false;
      }
      // We do not clear rolling_interned_data, as it is globally scoped.
      rolling_profile_packets_by_seq.erase(seq_id);
    }
  });

  if (!rolling_profile_packets_by_seq.empty()) {
    PERFETTO_ELOG("WARNING: Truncated heap dump. Not generating profile.");
    return false;
  }
  return success;
}

}  // namespace trace_to_text
}  // namespace perfetto
