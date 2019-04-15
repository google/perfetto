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

#include "tools/trace_to_text/trace_to_profile.h"

#include <cxxabi.h>
#include <inttypes.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "tools/trace_to_text/utils.h"

#include "perfetto/base/file_utils.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/temp_file.h"

#include "perfetto/trace/profiling/profile_packet.pb.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

#include "third_party/pprof/profile.pb.h"

namespace perfetto {
namespace trace_to_text {

namespace {

constexpr const char* kDefaultTmp = "/tmp";

void MaybeDemangle(std::string* name) {
  int ignored;
  char* data = abi::__cxa_demangle(name->c_str(), nullptr, nullptr, &ignored);
  if (data) {
    *name = data;
    free(data);
  }
}

std::string GetTemp() {
  const char* tmp = getenv("TMPDIR");
  if (tmp == nullptr)
    tmp = kDefaultTmp;
  return tmp;
}

using ::perfetto::protos::ProfilePacket;

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
  kBytes
};

void DumpProfilePacket(std::vector<ProfilePacket>& packet_fragments,
                       const std::string& file_prefix) {
  std::map<uint64_t, std::string> string_lookup;
  // A profile packet can be split into multiple fragments. We need to iterate
  // over all of them to reconstruct the original packet.
  for (const ProfilePacket& packet : packet_fragments) {
    for (const ProfilePacket::InternedString& interned_string :
         packet.strings())
      string_lookup.emplace(interned_string.id(), interned_string.str());
  }

  std::map<uint64_t, const std::vector<uint64_t>> callstack_lookup;
  for (const ProfilePacket& packet : packet_fragments) {
    for (const ProfilePacket::Callstack& callstack : packet.callstacks()) {
      std::vector<uint64_t> frame_ids(
          static_cast<size_t>(callstack.frame_ids().size()));
      std::reverse_copy(callstack.frame_ids().cbegin(),
                        callstack.frame_ids().cend(), frame_ids.begin());
      callstack_lookup.emplace(callstack.id(), std::move(frame_ids));
    }
  }

  std::map<std::string, uint64_t> string_table;
  string_table[""] = kEmpty;
  string_table["objects"] = kObjects;
  string_table["alloc_objects"] = kAllocObjects;
  string_table["count"] = kCount;
  string_table["space"] = kSpace;
  string_table["alloc_space"] = kAllocSpace;
  string_table["bytes"] = kBytes;

  GProfile profile;
  GValueType* value_type = profile.add_sample_type();
  value_type->set_type(kObjects);
  value_type->set_unit(kCount);

  value_type = profile.add_sample_type();
  value_type->set_type(kAllocObjects);
  value_type->set_unit(kCount);

  value_type = profile.add_sample_type();
  value_type->set_type(kAllocSpace);
  value_type->set_unit(kBytes);

  // The last value is the default one selected.
  value_type = profile.add_sample_type();
  value_type->set_type(kSpace);
  value_type->set_unit(kBytes);

  for (const ProfilePacket& packet : packet_fragments) {
    for (const ProfilePacket::Mapping& mapping : packet.mappings()) {
      GMapping* gmapping = profile.add_mapping();
      gmapping->set_id(mapping.id());
      gmapping->set_memory_start(mapping.start());
      gmapping->set_memory_limit(mapping.end());
      gmapping->set_file_offset(mapping.offset());
      std::string filename;
      for (uint64_t str_id : mapping.path_string_ids()) {
        auto it = string_lookup.find(str_id);
        if (it == string_lookup.end()) {
          PERFETTO_ELOG("Mapping %" PRIu64
                        " referring to invalid string_id %" PRIu64 ".",
                        static_cast<uint64_t>(mapping.id()), str_id);
          continue;
        }

        filename += "/" + it->second;
      }

      decltype(string_table)::iterator it;
      std::tie(it, std::ignore) =
          string_table.emplace(filename, string_table.size());
      gmapping->set_filename(static_cast<int64_t>(it->second));

      auto str_it = string_lookup.find(mapping.build_id());
      if (str_it != string_lookup.end()) {
        const std::string& build_id = str_it->second;
        std::tie(it, std::ignore) =
            string_table.emplace(ToHex(build_id), string_table.size());
        gmapping->set_build_id(static_cast<int64_t>(it->second));
      }
    }
  }

  std::set<uint64_t> functions_to_dump;
  for (const ProfilePacket& packet : packet_fragments) {
    for (const ProfilePacket::Frame& frame : packet.frames()) {
      GLocation* glocation = profile.add_location();
      glocation->set_id(frame.id());
      glocation->set_mapping_id(frame.mapping_id());
      // TODO(fmayer): This is probably incorrect. Probably should be abs pc.
      glocation->set_address(frame.rel_pc());
      GLine* gline = glocation->add_line();
      gline->set_function_id(frame.function_name_id());
      functions_to_dump.emplace(frame.function_name_id());
    }
  }

  for (uint64_t function_name_id : functions_to_dump) {
    auto str_it = string_lookup.find(function_name_id);
    if (str_it == string_lookup.end()) {
      PERFETTO_ELOG("Function referring to invalid string id %" PRIu64,
                    function_name_id);
      continue;
    }
    decltype(string_table)::iterator it;
    std::string function_name = str_it->second;
    // This assumes both the device that captured the trace and the host
    // machine use the same mangling scheme. This is a reasonable
    // assumption as the Itanium ABI is the de-facto standard for mangling.
    MaybeDemangle(&function_name);
    std::tie(it, std::ignore) =
        string_table.emplace(std::move(function_name), string_table.size());
    GFunction* gfunction = profile.add_function();
    gfunction->set_id(function_name_id);
    gfunction->set_name(static_cast<int64_t>(it->second));
  }

  // We keep the interning table as string -> uint64_t for fast and easy
  // lookup. When dumping, we need to turn it into a uint64_t -> string
  // table so we get it sorted by key order.
  std::map<uint64_t, std::string> inverted_string_table;
  for (const auto& p : string_table)
    inverted_string_table[p.second] = p.first;
  for (const auto& p : inverted_string_table)
    profile.add_string_table(p.second);

  std::map<uint64_t, std::vector<const ProfilePacket::ProcessHeapSamples*>>
      heap_samples;
  for (const ProfilePacket& packet : packet_fragments) {
    for (const ProfilePacket::ProcessHeapSamples& samples :
         packet.process_dumps()) {
      heap_samples[samples.pid()].emplace_back(&samples);
    }
  }
  for (const auto& p : heap_samples) {
    GProfile cur_profile = profile;
    uint64_t pid = p.first;
    for (const ProfilePacket::ProcessHeapSamples* samples : p.second) {
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
        auto it = callstack_lookup.find(sample.callstack_id());
        if (it == callstack_lookup.end()) {
          PERFETTO_ELOG("Callstack referring to invalid callstack id %" PRIu64,
                        static_cast<uint64_t>(sample.callstack_id()));
          continue;
        }
        for (uint64_t frame_id : it->second)
          gsample->add_location_id(frame_id);
        gsample->add_value(
            static_cast<int64_t>(sample.alloc_count() - sample.free_count()));
        gsample->add_value(static_cast<int64_t>(sample.alloc_count()));
        gsample->add_value(static_cast<int64_t>(sample.self_allocated()));
        gsample->add_value(static_cast<int64_t>(sample.self_allocated() -
                                                sample.self_freed()));
      }
    }
    std::string filename = file_prefix + std::to_string(pid) + ".pb";
    base::ScopedFile fd(base::OpenFile(filename, O_CREAT | O_WRONLY, 0700));
    if (!fd)
      PERFETTO_FATAL("Failed to open %s", filename.c_str());
    std::string serialized = cur_profile.SerializeAsString();
    PERFETTO_CHECK(base::WriteAll(*fd, serialized.c_str(), serialized.size()) ==
                   static_cast<ssize_t>(serialized.size()));
  }
}

}  // namespace

int TraceToProfile(std::istream* input, std::ostream* output) {
  std::string temp_dir = GetTemp() + "/heap_profile-XXXXXXX";
  size_t itr = 0;
  PERFETTO_CHECK(mkdtemp(&temp_dir[0]));
  std::vector<ProfilePacket> rolling_profile_packets;
  ForEachPacketInTrace(input, [&temp_dir, &itr, &rolling_profile_packets](
                                  const protos::TracePacket& packet) {
    if (!packet.has_profile_packet())
      return;
    rolling_profile_packets.emplace_back(packet.profile_packet());
    if (!packet.profile_packet().continued()) {
      for (size_t i = 1; i < rolling_profile_packets.size(); ++i) {
        // Ensure we are not missing a chunk.
        PERFETTO_CHECK(rolling_profile_packets[i - 1].index() + 1 ==
                       rolling_profile_packets[i].index());
      }
      DumpProfilePacket(rolling_profile_packets,
                        temp_dir + "/heap_dump." + std::to_string(++itr) + ".");
      rolling_profile_packets.clear();
    }
  });

  if (!rolling_profile_packets.empty()) {
    *output << "WARNING: Truncated heap dump. Not generating profile."
            << std::endl;
  }

  *output << "Wrote profiles to " << temp_dir << std::endl;

  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
