/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/profiling/profile_builder.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/trace_processor/basic_types.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <cxxabi.h>
#endif

namespace {
using StringId = ::perfetto::trace_processor::StringPool::Id;

// In-memory representation of a Profile.Function.
struct Function {
  StringId name_id = StringId::Null();
  StringId system_name_id = StringId::Null();
  StringId filename_id = StringId::Null();

  Function(StringId n, StringId s, StringId f)
      : name_id(n), system_name_id(s), filename_id(f) {}

  bool operator==(const Function& other) const {
    return std::tie(name_id, system_name_id, filename_id) ==
           std::tie(other.name_id, other.system_name_id, other.filename_id);
  }
};

// In-memory representation of a Profile.Line.
struct Line {
  int64_t function_id = 0;  // LocationTracker's interned Function id
  int64_t line_no = 0;

  Line(int64_t func, int64_t line) : function_id(func), line_no(line) {}

  bool operator==(const Line& other) const {
    return function_id == other.function_id && line_no == other.line_no;
  }
};

// In-memory representation of a Profile.Location.
struct Location {
  int64_t mapping_id = 0;  // sqlite row id
  // Common case: location references a single function.
  int64_t single_function_id = 0;  // interned Function id
  // Alternatively: multiple inlined functions, recovered via offline
  // symbolisation. Leaf-first ordering.
  std::vector<Line> inlined_functions;

  Location(int64_t map, int64_t func, std::vector<Line> inlines)
      : mapping_id(map),
        single_function_id(func),
        inlined_functions(std::move(inlines)) {}

  bool operator==(const Location& other) const {
    return std::tie(mapping_id, single_function_id, inlined_functions) ==
           std::tie(other.mapping_id, other.single_function_id,
                    other.inlined_functions);
  }
};
}  // namespace

template <>
struct std::hash<Function> {
  size_t operator()(const Function& loc) const {
    perfetto::base::Hash hasher;
    hasher.Update(loc.name_id.raw_id());
    hasher.Update(loc.system_name_id.raw_id());
    hasher.Update(loc.filename_id.raw_id());
    return static_cast<size_t>(hasher.digest());
  }
};

template <>
struct std::hash<Location> {
  size_t operator()(const Location& loc) const {
    perfetto::base::Hash hasher;
    hasher.Update(loc.mapping_id);
    hasher.Update(loc.single_function_id);
    for (auto line : loc.inlined_functions) {
      hasher.Update(line.function_id);
      hasher.Update(line.line_no);
    }
    return static_cast<size_t>(hasher.digest());
  }
};

namespace perfetto {
namespace profiling {
namespace {

using ::perfetto::trace_processor::Iterator;

uint64_t ToPprofId(int64_t id) {
  PERFETTO_DCHECK(id >= 0);
  return static_cast<uint64_t>(id) + 1;
}

struct PreprocessedInline {
  // |name_id| is already demangled
  StringId name_id = StringId::Null();
  StringId filename_id = StringId::Null();
  int64_t line_no = 0;

  PreprocessedInline(StringId s, StringId f, int64_t line)
      : name_id(s), filename_id(f), line_no(line) {}
};

std::unordered_map<int64_t, std::vector<PreprocessedInline>>
PreprocessInliningInfo(trace_processor::TraceProcessor* tp,
                       trace_processor::StringPool* interner) {
  std::unordered_map<int64_t, std::vector<PreprocessedInline>> inlines;

  // Most-inlined function (leaf) has the lowest id within a symbol set. Query
  // such that the per-set line vectors are built up leaf-first.
  Iterator it = tp->ExecuteQuery(
      "select symbol_set_id, name, source_file, line_number from "
      "stack_profile_symbol order by symbol_set_id asc, id asc;");
  while (it.Next()) {
    int64_t symbol_set_id = it.Get(0).AsLong();
    auto func_sysname = it.Get(1).is_null() ? "" : it.Get(1).AsString();
    auto filename = it.Get(2).is_null() ? "" : it.Get(2).AsString();
    int64_t line_no = it.Get(3).AsLong();

    inlines[symbol_set_id].emplace_back(interner->InternString(func_sysname),
                                        interner->InternString(filename),
                                        line_no);
  }

  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            it.Status().message().c_str());
    return {};
  }
  return inlines;
}
}  // namespace

// Interns Locations, Lines, and Functions. Interning is done by the entity's
// contents, and has no relation to the row ids in the SQL tables.
// Contains all data for the trace, so can be reused when emitting multiple
// profiles.
//
// TODO(rsavitski): consider moving mappings into here as well. For now, they're
// still emitted in a single scan during profile building. Mappings should be
// unique-enough already in the SQL tables, with only incremental state clearing
// duplicating entries.
class GProfileBuilder::LocationTracker {
 public:
  int64_t InternLocation(Location loc) {
    auto it = locations_.find(loc);
    if (it == locations_.end()) {
      bool inserted = false;
      std::tie(it, inserted) = locations_.emplace(
          std::move(loc), static_cast<int64_t>(locations_.size()));
      PERFETTO_DCHECK(inserted);
    }
    return it->second;
  }

  int64_t InternFunction(Function func) {
    auto it = functions_.find(func);
    if (it == functions_.end()) {
      bool inserted = false;
      std::tie(it, inserted) =
          functions_.emplace(func, static_cast<int64_t>(functions_.size()));
      PERFETTO_DCHECK(inserted);
    }
    return it->second;
  }

  bool IsCallsiteProcessed(int64_t callstack_id) const {
    return callsite_to_locations_.find(callstack_id) !=
           callsite_to_locations_.end();
  }

  void MaybeSetCallsiteLocations(int64_t callstack_id,
                                 const std::vector<int64_t>& locs) {
    // nop if already set
    callsite_to_locations_.emplace(callstack_id, locs);
  }

  const std::vector<int64_t>& LocationsForCallstack(
      int64_t callstack_id) const {
    auto it = callsite_to_locations_.find(callstack_id);
    PERFETTO_CHECK(callstack_id >= 0 && it != callsite_to_locations_.end());
    return it->second;
  }

  const std::unordered_map<Location, int64_t>& AllLocations() const {
    return locations_;
  }
  const std::unordered_map<Function, int64_t>& AllFunctions() const {
    return functions_;
  }

 private:
  // Root-first location ids for a given callsite id.
  std::unordered_map<int64_t, std::vector<int64_t>> callsite_to_locations_;
  std::unordered_map<Location, int64_t> locations_;
  std::unordered_map<Function, int64_t> functions_;
};

std::unique_ptr<GProfileBuilder::LocationTracker>
GProfileBuilder::PreprocessLocations(trace_processor::TraceProcessor* tp,
                                     trace_processor::StringPool* interner,
                                     bool annotate_frames) {
  std::unique_ptr<GProfileBuilder::LocationTracker> tracker(
      new LocationTracker());

  // Keyed by symbol_set_id, discarded once this function converts the inlines
  // into Line and Function entries.
  std::unordered_map<int64_t, std::vector<PreprocessedInline>> inlining_info =
      PreprocessInliningInfo(tp, interner);

  // Higher callsite ids most likely correspond to the deepest stacks, so we'll
  // fill more of the overall callsite->location map by visiting the callsited
  // in decreasing id order. Since processing a callstack also fills in the data
  // for all parent callsites.
  Iterator cid_it = tp->ExecuteQuery(
      "select id from stack_profile_callsite order by id desc;");
  while (cid_it.Next()) {
    int64_t query_cid = cid_it.Get(0).AsLong();

    // If the leaf has been processed, the rest of the stack is already known.
    if (tracker->IsCallsiteProcessed(query_cid))
      continue;

    std::string annotated_query =
        "select sp.id, sp.annotation, spf.mapping, spf.name, "
        "coalesce(spf.deobfuscated_name, demangle(spf.name), spf.name), "
        "spf.symbol_set_id from "
        "experimental_annotated_callstack(" +
        std::to_string(query_cid) +
        ") sp join stack_profile_frame spf on (sp.frame_id == spf.id) "
        "order by depth asc";
    Iterator c_it = tp->ExecuteQuery(annotated_query);

    std::vector<int64_t> callstack_loc_ids;
    while (c_it.Next()) {
      int64_t cid = c_it.Get(0).AsLong();
      auto annotation = c_it.Get(1).is_null() ? "" : c_it.Get(1).AsString();
      int64_t mapping_id = c_it.Get(2).AsLong();
      auto func_sysname = c_it.Get(3).is_null() ? "" : c_it.Get(3).AsString();
      auto func_name = c_it.Get(4).is_null() ? "" : c_it.Get(4).AsString();
      base::Optional<int64_t> symbol_set_id =
          c_it.Get(5).is_null() ? base::nullopt
                                : base::make_optional(c_it.Get(5).AsLong());

      Location loc(mapping_id, /*single_function_id=*/-1, {});

      auto intern_function = [interner, &tracker, annotate_frames](
                                 StringId func_sysname_id,
                                 StringId original_func_name_id,
                                 StringId filename_id,
                                 const std::string& anno) {
        std::string fname = interner->Get(original_func_name_id).ToStdString();
        if (annotate_frames && !anno.empty() && !fname.empty())
          fname = fname + " [" + anno + "]";
        StringId func_name_id = interner->InternString(base::StringView(fname));
        Function func(func_name_id, func_sysname_id, filename_id);
        return tracker->InternFunction(func);
      };

      // Inlining information available
      if (symbol_set_id.has_value()) {
        auto it = inlining_info.find(*symbol_set_id);
        if (it == inlining_info.end()) {
          PERFETTO_DFATAL_OR_ELOG(
              "Failed to find stack_profile_symbol entry for symbol_set_id "
              "%" PRIi64 "",
              *symbol_set_id);
          return {};
        }

        // N inlined functions
        // The symbolised packets currently assume pre-demangled data (as that's
        // the default of llvm-symbolizer), so we don't have a system name for
        // each deinlined frame. Set the human-readable name for both fields. We
        // can change this, but there's no demand for accurate system names in
        // pprofs.
        for (const auto& line : it->second) {
          int64_t func_id = intern_function(line.name_id, line.name_id,
                                            line.filename_id, annotation);

          loc.inlined_functions.emplace_back(func_id, line.line_no);
        }
      } else {
        // Otherwise - single function
        int64_t func_id =
            intern_function(interner->InternString(func_sysname),
                            interner->InternString(func_name),
                            /*filename_id=*/StringId::Null(), annotation);
        loc.single_function_id = func_id;
      }

      int64_t loc_id = tracker->InternLocation(std::move(loc));

      // Update the tracker with the locations so far (for example, at depth 2,
      // we'll have 3 root-most locations in |callstack_loc_ids|).
      callstack_loc_ids.push_back(loc_id);
      tracker->MaybeSetCallsiteLocations(cid, callstack_loc_ids);
    }

    if (!c_it.Status().ok()) {
      PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                              c_it.Status().message().c_str());
      return {};
    }
  }

  if (!cid_it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            cid_it.Status().message().c_str());
    return {};
  }

  return tracker;
}

GProfileBuilder::GProfileBuilder(trace_processor::TraceProcessor* tp,
                                 bool annotate_frames)
    : trace_processor_(*tp),
      interner_(),
      locations_(PreprocessLocations(tp, &interner_, annotate_frames)) {
  Reset();
}

GProfileBuilder::~GProfileBuilder() = default;

void GProfileBuilder::Reset() {
  interning_remapper_.clear();
  string_table_.clear();
  result_.Reset();
  seen_locations_.clear();
  // The pprof format requires the first entry in the string table to be the
  // empty string.
  int64_t empty_id = ToStringTableId(StringId::Null());
  PERFETTO_CHECK(empty_id == 0);
}

void GProfileBuilder::WriteSampleTypes(
    const std::vector<std::pair<std::string, std::string>>& sample_types) {
  for (const auto& st : sample_types) {
    auto* sample_type = result_->add_sample_type();
    sample_type->set_type(
        ToStringTableId(interner_.InternString(base::StringView(st.first))));
    sample_type->set_unit(
        ToStringTableId(interner_.InternString(base::StringView(st.second))));
  }
}

bool GProfileBuilder::AddSample(const protozero::PackedVarInt& values,
                                int64_t callstack_id) {
  const auto& location_ids = locations_->LocationsForCallstack(callstack_id);
  if (location_ids.empty()) {
    PERFETTO_DFATAL_OR_ELOG(
        "Failed to find frames for callstack id %" PRIi64 "", callstack_id);
    return false;
  }

  // LocationTracker stores location lists root-first, but the pprof format
  // requires leaf-first.
  protozero::PackedVarInt packed_locs;
  for (auto it = location_ids.rbegin(); it != location_ids.rend(); ++it)
    packed_locs.Append(ToPprofId(*it));

  auto* gsample = result_->add_sample();
  gsample->set_value(values);
  gsample->set_location_id(packed_locs);

  // Remember the locations s.t. we only serialize the referenced ones.
  seen_locations_.insert(location_ids.cbegin(), location_ids.cend());
  return true;
}

std::string GProfileBuilder::CompleteProfile() {
  std::set<int64_t> seen_mappings;
  std::set<int64_t> seen_functions;

  if (!WriteLocations(&seen_mappings, &seen_functions))
    return {};
  if (!WriteFunctions(seen_functions))
    return {};
  if (!WriteMappings(seen_mappings))
    return {};

  WriteStringTable();
  return result_.SerializeAsString();
}

bool GProfileBuilder::WriteLocations(std::set<int64_t>* seen_mappings,
                                     std::set<int64_t>* seen_functions) {
  const std::unordered_map<Location, int64_t>& locations =
      locations_->AllLocations();

  size_t written_locations = 0;
  for (const auto& loc_and_id : locations) {
    const auto& loc = loc_and_id.first;
    int64_t id = loc_and_id.second;

    if (seen_locations_.find(id) == seen_locations_.end())
      continue;

    written_locations += 1;
    seen_mappings->emplace(loc.mapping_id);

    auto* glocation = result_->add_location();
    glocation->set_id(ToPprofId(id));
    glocation->set_mapping_id(ToPprofId(loc.mapping_id));

    if (!loc.inlined_functions.empty()) {
      for (const auto& line : loc.inlined_functions) {
        seen_functions->insert(line.function_id);

        auto* gline = glocation->add_line();
        gline->set_function_id(ToPprofId(line.function_id));
        gline->set_line(line.line_no);
      }
    } else {
      seen_functions->insert(loc.single_function_id);

      glocation->add_line()->set_function_id(ToPprofId(loc.single_function_id));
    }
  }

  if (written_locations != seen_locations_.size()) {
    PERFETTO_DFATAL_OR_ELOG(
        "Found only %zu/%zu locations during serialization.", written_locations,
        seen_locations_.size());
    return false;
  }
  return true;
}

bool GProfileBuilder::WriteFunctions(const std::set<int64_t>& seen_functions) {
  const std::unordered_map<Function, int64_t>& functions =
      locations_->AllFunctions();

  size_t written_functions = 0;
  for (const auto& func_and_id : functions) {
    const auto& func = func_and_id.first;
    int64_t id = func_and_id.second;

    if (seen_functions.find(id) == seen_functions.end())
      continue;

    written_functions += 1;

    auto* gfunction = result_->add_function();
    gfunction->set_id(ToPprofId(id));
    gfunction->set_name(ToStringTableId(func.name_id));
    gfunction->set_system_name(ToStringTableId(func.system_name_id));
    if (!func.filename_id.is_null())
      gfunction->set_filename(ToStringTableId(func.filename_id));
  }

  if (written_functions != seen_functions.size()) {
    PERFETTO_DFATAL_OR_ELOG(
        "Found only %zu/%zu functions during serialization.", written_functions,
        seen_functions.size());
    return false;
  }
  return true;
}

bool GProfileBuilder::WriteMappings(const std::set<int64_t>& seen_mappings) {
  Iterator mapping_it = trace_processor_.ExecuteQuery(
      "SELECT id, exact_offset, start, end, name "
      "FROM stack_profile_mapping;");
  size_t mappings_no = 0;
  while (mapping_it.Next()) {
    int64_t id = mapping_it.Get(0).AsLong();
    if (seen_mappings.find(id) == seen_mappings.end())
      continue;
    ++mappings_no;
    auto interned_filename =
        ToStringTableId(interner_.InternString(mapping_it.Get(4).AsString()));
    auto* gmapping = result_->add_mapping();
    gmapping->set_id(ToPprofId(id));
    // Do not set the build_id here to avoid downstream services
    // trying to symbolize (e.g. b/141735056)
    gmapping->set_file_offset(
        static_cast<uint64_t>(mapping_it.Get(1).AsLong()));
    gmapping->set_memory_start(
        static_cast<uint64_t>(mapping_it.Get(2).AsLong()));
    gmapping->set_memory_limit(
        static_cast<uint64_t>(mapping_it.Get(3).AsLong()));
    gmapping->set_filename(interned_filename);
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

void GProfileBuilder::WriteStringTable() {
  for (StringId id : string_table_) {
    trace_processor::NullTermStringView s = interner_.Get(id);
    result_->add_string_table(s.data(), s.size());
  }
}

int64_t GProfileBuilder::ToStringTableId(StringId interned_id) {
  auto it = interning_remapper_.find(interned_id);
  if (it == interning_remapper_.end()) {
    int64_t table_id = static_cast<int64_t>(string_table_.size());
    string_table_.push_back(interned_id);
    bool inserted = false;
    std::tie(it, inserted) = interning_remapper_.emplace(interned_id, table_id);
    PERFETTO_DCHECK(inserted);
  }
  return it->second;
}

}  // namespace profiling
}  // namespace perfetto
