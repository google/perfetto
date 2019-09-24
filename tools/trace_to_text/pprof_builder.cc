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

#include "perfetto/profiling/pprof_builder.h"

#include <cxxabi.h>
#include <inttypes.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "tools/trace_to_text/utils.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/profiling/symbolizer.h"

#include "protos/perfetto/trace/profiling/profile_common.pb.h"
#include "protos/perfetto/trace/profiling/profile_packet.pb.h"
#include "protos/perfetto/trace/trace.pb.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/pprof/profile.pb.h"

namespace perfetto {
namespace trace_to_text {

namespace {

using ::protozero::proto_utils::kMessageLengthFieldSize;
using ::protozero::proto_utils::MakeTagLengthDelimited;
using ::protozero::proto_utils::WriteVarInt;

using GLine = ::perfetto::third_party::perftools::profiles::Line;
using GMapping = ::perfetto::third_party::perftools::profiles::Mapping;
using GLocation = ::perfetto::third_party::perftools::profiles::Location;
using GProfile = ::perfetto::third_party::perftools::profiles::Profile;
using GValueType = ::perfetto::third_party::perftools::profiles::ValueType;
using GFunction = ::perfetto::third_party::perftools::profiles::Function;
using GSample = ::perfetto::third_party::perftools::profiles::Sample;

struct View {
  const char* type;
  const char* unit;
  const char* aggregator;
  const char* filter;
};

void MaybeDemangle(std::string* name) {
  int ignored;
  char* data = abi::__cxa_demangle(name->c_str(), nullptr, nullptr, &ignored);
  if (data) {
    *name = data;
    free(data);
  }
}

const View kSpaceView{"space", "bytes", "SUM(size)", nullptr};
const View kAllocSpaceView{"alloc_space", "bytes", "SUM(size)", "size > 0"};
const View kAllocObjectsView{"alloc_objects", "count", "sum(count)",
                             "size > 0"};
const View kObjectsView{"objects", "count", "SUM(count)", nullptr};

const View kViews[] = {kAllocObjectsView, kObjectsView, kAllocSpaceView,
                       kSpaceView};

using Iterator = trace_processor::TraceProcessor::Iterator;

constexpr const char* kQueryProfiles =
    "select distinct hpa.upid, hpa.ts from heap_profile_allocation hpa;";

struct Callsite {
  int64_t id;
  int64_t frame_id;
};

// Walk tree bottom up and assign the inverse of the frame_ids of the path
// that was used to reach each node into result.
void Walk(const std::vector<std::vector<Callsite>> children_map,
          std::vector<std::vector<int64_t>>* result,
          std::vector<int64_t> parents,
          const Callsite& root) {
  PERFETTO_DCHECK((*result)[static_cast<size_t>(root.id)].empty());
  parents.push_back(root.frame_id);
  // pprof stores the frames the other way round that we do, reverse here.
  (*result)[static_cast<size_t>(root.id)].assign(parents.rbegin(),
                                                 parents.rend());
  const std::vector<Callsite>& children =
      children_map[static_cast<size_t>(root.id)];
  for (const Callsite& child : children)
    Walk(children_map, result, parents, child);
}

// Return map from callsite_id to list of frame_ids that make up the callstack.
std::vector<std::vector<int64_t>> GetCallsiteToFrames(
    trace_processor::TraceProcessor* tp) {
  Iterator count_it =
      tp->ExecuteQuery("select count(*) from stack_profile_callsite;");
  if (!count_it.Next()) {
    PERFETTO_DFATAL_OR_ELOG("Failed to get number of callsites: %s",
                            count_it.Status().message().c_str());
    return {};
  }
  int64_t count = count_it.Get(0).long_value;
  std::vector<std::vector<Callsite>> children(static_cast<size_t>(count));

  Iterator it = tp->ExecuteQuery(
      "select id, parent_id, frame_id from stack_profile_callsite;");
  std::vector<Callsite> roots;
  while (it.Next()) {
    int64_t id = it.Get(0).long_value;
    int64_t parent_id = it.Get(1).long_value;
    int64_t frame_id = it.Get(2).long_value;
    Callsite callsite{id, frame_id};
    if (parent_id == -1)
      roots.emplace_back(callsite);
    else
      children[static_cast<size_t>(parent_id)].emplace_back(callsite);
  }

  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            it.Status().message().c_str());
    return {};
  }

  std::vector<std::vector<int64_t>> result(static_cast<size_t>(count));
  auto start = base::GetWallTimeMs();
  for (const Callsite& root : roots)
    Walk(children, &result, {}, root);
  PERFETTO_DLOG("Walked %zu in %llu", children.size(),
                (base::GetWallTimeMs() - start).count());
  return result;
}

struct Line {
  int64_t symbol_id;
  uint32_t line_number;
};

std::map<int64_t, std::vector<Line>> GetSymbolSetIdToLines(
    trace_processor::TraceProcessor* tp) {
  std::map<int64_t, std::vector<Line>> result;
  Iterator it = tp->ExecuteQuery(
      "SELECT symbol_set_id, id, line_number FROM stack_profile_symbol;");
  while (it.Next()) {
    int64_t symbol_set_id = it.Get(0).long_value;
    int64_t id = it.Get(1).long_value;
    int64_t line_number = it.Get(2).long_value;
    result[symbol_set_id].emplace_back(
        Line{id, static_cast<uint32_t>(line_number)});
  }

  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            it.Status().message().c_str());
    return {};
  }
  return result;
}

class GProfileBuilder {
 public:
  GProfileBuilder(
      const std::vector<std::vector<int64_t>>& callsite_to_frames,
      const std::map<int64_t, std::vector<Line>>& symbol_set_id_to_lines,
      int64_t max_symbol_id)
      : callsite_to_frames_(callsite_to_frames),
        symbol_set_id_to_lines_(symbol_set_id_to_lines),
        max_symbol_id_(max_symbol_id) {
    // The pprof format expects the first entry in the string table to be the
    // empty string.
    Intern("");
  }

  std::vector<Iterator> BuildViewIterators(trace_processor::TraceProcessor* tp,
                                           uint64_t upid,
                                           uint64_t ts) {
    std::vector<Iterator> view_its;
    for (size_t i = 0; i < base::ArraySize(kViews); ++i) {
      const View& v = kViews[i];
      std::string query = "SELECT hpa.callsite_id ";
      query += ", " + std::string(v.aggregator) +
               " FROM heap_profile_allocation hpa ";
      query += "WHERE hpa.upid = " + std::to_string(upid) + " ";
      query += "AND hpa.ts <= " + std::to_string(ts) + " ";
      if (v.filter)
        query += "AND " + std::string(v.filter) + " ";
      query += "GROUP BY hpa.callsite_id;";
      view_its.emplace_back(tp->ExecuteQuery(query));
    }
    return view_its;
  }

  bool WriteAllocations(std::vector<Iterator>* view_its,
                        std::set<int64_t>* seen_frames) {
    for (;;) {
      bool all_next = true;
      bool any_next = false;
      for (size_t i = 0; i < base::ArraySize(kViews); ++i) {
        Iterator& it = (*view_its)[i];
        bool next = it.Next();
        if (!it.Status().ok()) {
          PERFETTO_DFATAL_OR_ELOG("Invalid view iterator: %s",
                                  it.Status().message().c_str());
          return false;
        }
        all_next = all_next && next;
        any_next = any_next || next;
      }

      if (!all_next) {
        PERFETTO_DCHECK(!any_next);
        break;
      }

      GSample* gsample = result_.add_sample();
      for (size_t i = 0; i < base::ArraySize(kViews); ++i) {
        int64_t callstack_id = (*view_its)[i].Get(0).long_value;
        if (i == 0) {
          auto frames = FramesForCallstack(callstack_id);
          if (frames.empty())
            return false;
          for (int64_t frame : frames)
            gsample->add_location_id(ToPprofId(frame));
          seen_frames->insert(frames.cbegin(), frames.cend());
        } else {
          if (callstack_id != (*view_its)[i].Get(0).long_value) {
            PERFETTO_DFATAL_OR_ELOG("Wrong callstack.");
            return false;
          }
        }
        gsample->add_value((*view_its)[i].Get(1).long_value);
      }
    }
    return true;
  }

  bool WriteMappings(trace_processor::TraceProcessor* tp,
                     const std::set<int64_t> seen_mappings) {
    Iterator mapping_it = tp->ExecuteQuery(
        "SELECT id, build_id, exact_offset, start, end, name "
        "FROM stack_profile_mapping;");
    size_t mappings_no = 0;
    while (mapping_it.Next()) {
      int64_t id = mapping_it.Get(0).long_value;
      if (seen_mappings.find(id) == seen_mappings.end())
        continue;
      ++mappings_no;
      GMapping* gmapping = result_.add_mapping();
      gmapping->set_id(ToPprofId(id));
      gmapping->set_build_id(Intern(mapping_it.Get(1).string_value));
      gmapping->set_file_offset(
          static_cast<uint64_t>(mapping_it.Get(2).long_value));
      gmapping->set_memory_start(
          static_cast<uint64_t>(mapping_it.Get(3).long_value));
      gmapping->set_memory_limit(
          static_cast<uint64_t>(mapping_it.Get(4).long_value));
      gmapping->set_filename(Intern(mapping_it.Get(5).string_value));
    }
    if (!mapping_it.Status().ok()) {
      PERFETTO_DFATAL_OR_ELOG("Invalid mapping iterator: %s",
                              mapping_it.Status().message().c_str());
      return false;
    }
    if (mappings_no != seen_mappings.size()) {
      PERFETTO_DFATAL_OR_ELOG("Missing mappings.");
      return false;
    }
    return true;
  }

  bool WriteSymbols(trace_processor::TraceProcessor* tp,
                    const std::set<int64_t>& seen_symbol_ids) {
    Iterator symbol_it = tp->ExecuteQuery(
        "SELECT id, name, source_file FROM stack_profile_symbol");
    size_t symbols_no = 0;
    while (symbol_it.Next()) {
      int64_t id = symbol_it.Get(0).long_value;
      if (seen_symbol_ids.find(id) == seen_symbol_ids.end())
        continue;
      ++symbols_no;
      GFunction* gfunction = result_.add_function();
      gfunction->set_id(ToPprofId(id));
      gfunction->set_name(Intern(symbol_it.Get(1).string_value));
      gfunction->set_filename(Intern(symbol_it.Get(2).string_value));
    }

    if (!symbol_it.Status().ok()) {
      PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                              symbol_it.Status().message().c_str());
      return false;
    }

    if (symbols_no != seen_symbol_ids.size()) {
      PERFETTO_DFATAL_OR_ELOG("Missing symbols.");
      return false;
    }
    return true;
  }

  bool WriteFrames(trace_processor::TraceProcessor* tp,
                   const std::set<int64_t>& seen_frames,
                   std::set<int64_t>* seen_mappings,
                   std::set<int64_t>* seen_symbol_ids) {
    Iterator frame_it = tp->ExecuteQuery(
        "SELECT spf.id, spf.name, spf.mapping, spf.rel_pc, spf.symbol_set_id "
        "FROM stack_profile_frame spf;");
    size_t frames_no = 0;
    while (frame_it.Next()) {
      int64_t frame_id = frame_it.Get(0).long_value;
      if (seen_frames.find(frame_id) == seen_frames.end())
        continue;
      frames_no++;
      std::string frame_name = frame_it.Get(1).string_value;
      int64_t mapping_id = frame_it.Get(2).long_value;
      int64_t rel_pc = frame_it.Get(3).long_value;
      int64_t symbol_set_id = frame_it.Get(4).long_value;

      seen_mappings->emplace(mapping_id);
      GLocation* glocation = result_.add_location();
      glocation->set_id(ToPprofId(frame_id));
      glocation->set_mapping_id(ToPprofId(mapping_id));
      glocation->set_address(ToPprofId(rel_pc));
      if (symbol_set_id) {
        for (const Line& line : LineForSymbolSetId(symbol_set_id)) {
          seen_symbol_ids->emplace(line.symbol_id);
          GLine* gline = glocation->add_line();
          gline->set_line(line.line_number);
          gline->set_function_id(ToPprofId(line.symbol_id));
        }
      } else {
        int64_t synthesized_symbol_id = ++max_symbol_id_;
        std::string demangled_name = frame_name;
        MaybeDemangle(&demangled_name);

        GFunction* gfunction = result_.add_function();
        gfunction->set_id(ToPprofId(synthesized_symbol_id));
        gfunction->set_name(Intern(demangled_name));
        gfunction->set_system_name(Intern(frame_name));

        GLine* gline = glocation->add_line();
        gline->set_line(0);
        gline->set_function_id(ToPprofId(synthesized_symbol_id));
      }
    }

    if (!frame_it.Status().ok()) {
      PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                              frame_it.Status().message().c_str());
      return false;
    }
    if (frames_no != seen_frames.size()) {
      PERFETTO_DFATAL_OR_ELOG("Missing frames.");
      return false;
    }
    return true;
  }

  uint64_t ToPprofId(int64_t id) {
    PERFETTO_DCHECK(id >= 0);
    return static_cast<uint64_t>(id) + 1;
  }

  void WriteSampleTypes() {
    for (size_t i = 0; i < base::ArraySize(kViews); ++i) {
      auto* sample_type = result_.add_sample_type();
      sample_type->set_type(Intern(kViews[i].type));
      sample_type->set_unit(Intern(kViews[i].unit));
    }
  }

  GProfile GenerateGProfile(trace_processor::TraceProcessor* tp,
                            uint64_t upid,
                            uint64_t ts) {
    std::set<int64_t> seen_frames;
    std::set<int64_t> seen_mappings;
    std::set<int64_t> seen_symbol_ids;

    std::vector<Iterator> view_its = BuildViewIterators(tp, upid, ts);

    WriteSampleTypes();
    if (!WriteAllocations(&view_its, &seen_frames))
      return {};
    if (!WriteFrames(tp, seen_frames, &seen_mappings, &seen_symbol_ids))
      return {};
    if (!WriteMappings(tp, seen_mappings))
      return {};
    if (!WriteSymbols(tp, seen_symbol_ids))
      return {};
    return std::move(result_);
  }

  const std::vector<int64_t>& FramesForCallstack(int64_t callstack_id) {
    return callsite_to_frames_[static_cast<size_t>(callstack_id)];
  }

  const std::vector<Line>& LineForSymbolSetId(int64_t symbol_set_id) {
    auto it = symbol_set_id_to_lines_.find(symbol_set_id);
    if (it == symbol_set_id_to_lines_.end())
      return empty_line_vector_;
    return it->second;
  }

  int64_t Intern(const std::string& s) {
    auto it = string_table_.find(s);
    if (it == string_table_.end()) {
      std::tie(it, std::ignore) =
          string_table_.emplace(s, string_table_.size());
      result_.add_string_table(s);
    }
    return it->second;
  }

 private:
  GProfile result_;
  std::map<std::string, int64_t> string_table_;
  const std::vector<std::vector<int64_t>>& callsite_to_frames_;
  const std::map<int64_t, std::vector<Line>>& symbol_set_id_to_lines_;
  const std::vector<Line> empty_line_vector_;
  int64_t max_symbol_id_;
};

}  // namespace

bool TraceToPprof(std::istream* input,
                  std::vector<SerializedProfile>* output,
                  Symbolizer* symbolizer) {
  trace_processor::Config config;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);

  if (!ReadTrace(tp.get(), input))
    return 1;

  tp->NotifyEndOfFile();
  if (symbolizer) {
    SymbolizeDatabase(
        tp.get(), symbolizer, [&tp](perfetto::protos::TracePacket packet) {
          size_t size = static_cast<size_t>(packet.ByteSize());
          std::unique_ptr<uint8_t[]> buf(new uint8_t[size]);
          packet.SerializeToArray(buf.get(), packet.ByteSize());

          std::unique_ptr<uint8_t[]> preamble(new uint8_t[11]);
          preamble[0] =
              MakeTagLengthDelimited(protos::pbzero::Trace::kPacketFieldNumber);
          uint8_t* end = WriteVarInt(size, &preamble[1]);
          size_t preamble_size = static_cast<size_t>(end - &preamble[0]);
          auto status = tp->Parse(std::move(preamble), preamble_size);
          if (!status.ok()) {
            PERFETTO_DFATAL_OR_ELOG("Failed to parse: %s",
                                    status.message().c_str());
            return;
          }
          status = tp->Parse(std::move(buf), size);
          if (!status.ok()) {
            PERFETTO_DFATAL_OR_ELOG("Failed to parse: %s",
                                    status.message().c_str());
            return;
          }
        });
  }

  tp->NotifyEndOfFile();
  auto max_symbol_id_it =
      tp->ExecuteQuery("SELECT MAX(id) from stack_profile_symbol");
  if (!max_symbol_id_it.Next()) {
    PERFETTO_DFATAL_OR_ELOG("Failed to get max symbol set id: %s",
                            max_symbol_id_it.Status().message().c_str());
    return false;
  }

  int64_t max_symbol_id = max_symbol_id_it.Get(0).long_value;
  auto callsite_to_frames = GetCallsiteToFrames(tp.get());
  auto symbol_set_id_to_lines = GetSymbolSetIdToLines(tp.get());

  Iterator it = tp->ExecuteQuery(kQueryProfiles);
  while (it.Next()) {
    GProfileBuilder builder(callsite_to_frames, symbol_set_id_to_lines,
                            max_symbol_id);
    uint64_t upid = static_cast<uint64_t>(it.Get(0).long_value);
    uint64_t ts = static_cast<uint64_t>(it.Get(1).long_value);
    std::string pid_query = "select pid from process where upid = ";
    pid_query += std::to_string(upid) + ";";
    Iterator pid_it = tp->ExecuteQuery(pid_query);
    PERFETTO_CHECK(pid_it.Next());

    GProfile profile = builder.GenerateGProfile(tp.get(), upid, ts);
    output->emplace_back(
        SerializedProfile{static_cast<uint64_t>(pid_it.Get(0).long_value),
                          profile.SerializeAsString()});
  }
  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            it.Status().message().c_str());
    return false;
  }
  return true;
}

bool TraceToPprof(std::istream* input, std::vector<SerializedProfile>* output) {
  return TraceToPprof(input, output, nullptr);
}

}  // namespace trace_to_text
}  // namespace perfetto
