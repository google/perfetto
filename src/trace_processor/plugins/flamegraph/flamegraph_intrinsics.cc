/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/plugins/flamegraph/flamegraph_intrinsics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/regex.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/intrinsics/types/value.h"
#include "src/trace_processor/plugins/flamegraph/flamegraph.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_tagged_args.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
namespace {

using core::FlexVector;
using core::dataframe::AdhocDataframeBuilder;
using core::dataframe::NullabilityType;

constexpr char kFramesTag[] = "FLAMEGRAPH_FRAMES";
constexpr char kConfigTag[] = "FLAMEGRAPH_CONFIG";

// Formats a value the way SQLite renders it as text.
std::string NumberToText(int64_t v) {
  return std::to_string(v);
}
std::string NumberToText(double v) {
  base::StackString<32> s("%.15g", v);
  std::string res = s.ToStdString();
  if (res.find_first_of(".eEni") == std::string::npos) {
    res += ".0";
  }
  return res;
}

// Normalizes a double to a bit pattern where equal values (including
// -0.0 == 0.0 and any NaN == any NaN) have equal bits.
uint64_t DoubleBits(double d) {
  if (std::isnan(d)) {
    d = std::numeric_limits<double>::quiet_NaN();
  }
  uint64_t bits;
  memcpy(&bits, &d, sizeof(bits));
  if (bits == 0x8000000000000000ull) {
    bits = 0;
  }
  return bits;
}

// A property value with strings interned into the StringPool: equality is
// an O(1) comparison.
struct PropValue {
  enum class Kind : uint8_t { kNull, kInt, kDouble, kString };
  Kind kind = Kind::kNull;
  union {
    int64_t i;
    double d;
    uint32_t s;  // raw StringPool::Id
  };

  StringPool::Id string_id() const { return StringPool::Id::Raw(s); }

  bool operator==(const PropValue& o) const {
    if (kind != o.kind) {
      return false;
    }
    switch (kind) {
      case Kind::kNull:
        return true;
      case Kind::kInt:
        return i == o.i;
      case Kind::kDouble:
        return DoubleBits(d) == DoubleBits(o.d);
      case Kind::kString:
        return s == o.s;
    }
    PERFETTO_FATAL("For GCC");
  }

  std::string ToText(const StringPool& pool) const {
    switch (kind) {
      case Kind::kNull:
        return "";
      case Kind::kInt:
        return NumberToText(i);
      case Kind::kDouble:
        return NumberToText(d);
      case Kind::kString: {
        NullTermStringView v = pool.Get(string_id());
        return std::string(v.data(), v.size());
      }
    }
    PERFETTO_FATAL("For GCC");
  }
};

// The frames collected by __intrinsic_flamegraph_agg.
struct SqlFrames {
  static constexpr int64_t kNullParent = std::numeric_limits<int64_t>::max();

  struct PropertyColumn {
    std::string name;
    std::vector<perfetto_sql::Value> values;
  };

  std::vector<int64_t> ids;
  std::vector<int64_t> parent_ids;  // kNullParent for roots.
  std::vector<StringPool::Id> names;
  std::vector<double> values;
  bool all_values_integral = true;
  std::vector<PropertyColumn> properties;

  size_t size() const { return ids.size(); }
};

// The library config plus the property column plan, which the glue owns.
enum class AggMode : uint8_t { kOneOrSummary, kSum, kConcatWithComma };
struct SqlConfig {
  flamegraph::Config core;
  std::vector<std::string> grouping_columns;
  std::vector<std::pair<std::string, AggMode>> aggregate_columns;
};

struct AggCtx : sqlite::AggregateContext<AggCtx> {
  SqlFrames frames;
  bool initialized = false;
};

base::StatusOr<perfetto_sql::Value> ToValue(sqlite3_value* value) {
  switch (sqlite::value::Type(value)) {
    case sqlite::Type::kNull:
      return perfetto_sql::Value(std::monostate());
    case sqlite::Type::kInteger:
      return perfetto_sql::Value(sqlite::value::Int64(value));
    case sqlite::Type::kFloat:
      return perfetto_sql::Value(sqlite::value::Double(value));
    case sqlite::Type::kText:
      return perfetto_sql::Value(std::string(sqlite::value::Text(value)));
    case sqlite::Type::kBlob:
      return base::ErrStatus(
          "%s: blob property values are not supported", FlamegraphAgg::kName);
  }
  PERFETTO_FATAL("For GCC");
}

base::StatusOr<SqlConfig> ParseConfig(int argc, sqlite3_value** argv) {
  SqlConfig config;
  RETURN_IF_ERROR(sqlite::ParseTaggedArgs(
      FlamegraphConfig::kName, argc, argv,
      {
          {"view", 1,
           [&](sqlite3_value** v) -> base::Status {
             ASSIGN_OR_RETURN(std::string view,
                              sqlite::TaggedArgText(
                                  "__intrinsic_flamegraph_config: view",
                                  v[0]));
             if (view == "TOP_DOWN") {
               config.core.view = flamegraph::Config::View::kTopDown;
             } else if (view == "BOTTOM_UP") {
               config.core.view = flamegraph::Config::View::kBottomUp;
             } else if (view == "PIVOT") {
               config.core.view = flamegraph::Config::View::kPivot;
             } else {
               return base::ErrStatus(
                   "__intrinsic_flamegraph_config: unknown view '%s'",
                   view.c_str());
             }
             return base::OkStatus();
           }},
          {"pivot", 1,
           [&](sqlite3_value** v) -> base::Status {
             ASSIGN_OR_RETURN(std::string pattern,
                              sqlite::TaggedArgText(
                                  "__intrinsic_flamegraph_config: pivot",
                                  v[0]));
             config.core.pivot_pattern = std::move(pattern);
             return base::OkStatus();
           }},
          {"filter", 2,
           [&](sqlite3_value** v) -> base::Status {
             ASSIGN_OR_RETURN(std::string kind,
                              sqlite::TaggedArgText(
                                  "__intrinsic_flamegraph_config: filter",
                                  v[0]));
             ASSIGN_OR_RETURN(std::string pattern,
                              sqlite::TaggedArgText(
                                  "__intrinsic_flamegraph_config: filter",
                                  v[1]));
             flamegraph::Config::FilterKind k;
             if (kind == "SHOW_STACK") {
               k = flamegraph::Config::FilterKind::kShowStack;
             } else if (kind == "HIDE_STACK") {
               k = flamegraph::Config::FilterKind::kHideStack;
             } else if (kind == "SHOW_FROM_FRAME") {
               k = flamegraph::Config::FilterKind::kShowFromFrame;
             } else if (kind == "HIDE_FRAME") {
               k = flamegraph::Config::FilterKind::kHideFrame;
             } else {
               return base::ErrStatus(
                   "__intrinsic_flamegraph_config: unknown filter kind '%s'",
                   kind.c_str());
             }
             config.core.filters.push_back({k, std::move(pattern)});
             return base::OkStatus();
           }},
          {"grouping", 1,
           [&](sqlite3_value** v) -> base::Status {
             ASSIGN_OR_RETURN(std::string name,
                              sqlite::TaggedArgText(
                                  "__intrinsic_flamegraph_config: grouping",
                                  v[0]));
             config.grouping_columns.push_back(std::move(name));
             return base::OkStatus();
           }},
          {"aggregate", 2,
           [&](sqlite3_value** v) -> base::Status {
             ASSIGN_OR_RETURN(std::string name,
                              sqlite::TaggedArgText(
                                  "__intrinsic_flamegraph_config: aggregate",
                                  v[0]));
             ASSIGN_OR_RETURN(std::string mode,
                              sqlite::TaggedArgText(
                                  "__intrinsic_flamegraph_config: aggregate",
                                  v[1]));
             AggMode m;
             if (mode == "ONE_OR_SUMMARY") {
               m = AggMode::kOneOrSummary;
             } else if (mode == "SUM") {
               m = AggMode::kSum;
             } else if (mode == "CONCAT_WITH_COMMA") {
               m = AggMode::kConcatWithComma;
             } else {
               return base::ErrStatus(
                   "__intrinsic_flamegraph_config: unknown aggregate mode "
                   "'%s'",
                   mode.c_str());
             }
             config.aggregate_columns.emplace_back(std::move(name), m);
             return base::OkStatus();
           }},
      }));

  // Validate eagerly so mistakes surface at the config call even when the
  // input is empty.
  if (config.core.view == flamegraph::Config::View::kPivot &&
      !config.core.pivot_pattern) {
    return base::ErrStatus(
        "__intrinsic_flamegraph_config: 'pivot' is required for the PIVOT "
        "view");
  }
  if (config.core.view != flamegraph::Config::View::kPivot &&
      config.core.pivot_pattern) {
    return base::ErrStatus(
        "__intrinsic_flamegraph_config: 'pivot' is only valid for the PIVOT "
        "view");
  }
  for (const auto& f : config.core.filters) {
    RETURN_IF_ERROR(base::Regex::Create(f.pattern).status());
  }
  if (config.core.pivot_pattern) {
    RETURN_IF_ERROR(base::Regex::Create(*config.core.pivot_pattern).status());
  }
  return config;
}

// A property column with interned values.
struct PropColumn {
  std::string name;
  std::vector<PropValue> values;
};

PropColumn InternPropColumn(const SqlFrames::PropertyColumn& col,
                            StringPool* pool) {
  PropColumn res;
  res.name = col.name;
  res.values.reserve(col.values.size());
  for (const perfetto_sql::Value& v : col.values) {
    PropValue out;
    switch (v.index()) {
      case perfetto_sql::ValueIndex<std::monostate>():
        out.kind = PropValue::Kind::kNull;
        break;
      case perfetto_sql::ValueIndex<int64_t>():
        out.kind = PropValue::Kind::kInt;
        out.i = std::get<int64_t>(v);
        break;
      case perfetto_sql::ValueIndex<double>():
        out.kind = PropValue::Kind::kDouble;
        out.d = std::get<double>(v);
        break;
      case perfetto_sql::ValueIndex<std::string>():
        out.kind = PropValue::Kind::kString;
        out.s = pool->InternString(base::StringView(std::get<std::string>(v)))
                    .raw_id();
        break;
      default:
        PERFETTO_FATAL("Unknown value type");
    }
    res.values.push_back(out);
  }
  return res;
}

// Assigns dense ids to (name, grouping values) tuples through exact hash
// maps: one map per tuple position, so ids from different positions can
// never be confused.
class TupleInterner {
 public:
  explicit TupleInterner(size_t grouping_columns)
      : steps_(grouping_columns) {}

  // The dense id of the tuple so far, starting from the name and extended
  // by one grouping value per step.
  uint32_t Root(uint32_t name_raw_id) { return Intern(&roots_, name_raw_id); }
  uint32_t Extend(size_t step, uint32_t so_far, const PropValue& v) {
    return Intern(&steps_[step], (uint64_t(so_far) << 32) | ValueToken(v));
  }

 private:
  // A dense token for one property value, unique per distinct value.
  uint32_t ValueToken(const PropValue& v) {
    uint64_t raw;
    switch (v.kind) {
      case PropValue::Kind::kNull:
        raw = 0;
        break;
      case PropValue::Kind::kInt:
        raw = (uint64_t(1) << 62) | Intern(&ints_, uint64_t(v.i));
        break;
      case PropValue::Kind::kDouble:
        raw = (uint64_t(2) << 62) | Intern(&doubles_, DoubleBits(v.d));
        break;
      case PropValue::Kind::kString:
        raw = (uint64_t(3) << 62) | v.s;
        break;
    }
    return Intern(&tokens_, raw);
  }

  static uint32_t Intern(base::FlatHashMap<uint64_t, uint32_t>* map,
                         uint64_t key) {
    auto [it, inserted] = map->Insert(key, static_cast<uint32_t>(map->size()));
    base::ignore_result(inserted);
    return *it;
  }

  base::FlatHashMap<uint64_t, uint32_t> roots_;
  std::vector<base::FlatHashMap<uint64_t, uint32_t>> steps_;
  base::FlatHashMap<uint64_t, uint32_t> ints_;
  base::FlatHashMap<uint64_t, uint32_t> doubles_;
  base::FlatHashMap<uint64_t, uint32_t> tokens_;
};

base::StatusOr<core::dataframe::Dataframe> ComputeFlamegraph(
    const SqlFrames& frames,
    const SqlConfig& config,
    StringPool* pool) {
  auto n = static_cast<uint32_t>(frames.size());

  // Resolve the property columns by name; the output column order comes
  // from the config, not from the order properties were collected in.
  std::vector<PropColumn> prop_columns;
  std::vector<const PropColumn*> grouping_props;
  std::vector<const PropColumn*> agg_props;
  if (n != 0) {
    if (frames.properties.size() !=
        config.grouping_columns.size() + config.aggregate_columns.size()) {
      return base::ErrStatus(
          "flamegraph: collected frames have %zu property columns but the "
          "config specifies %zu",
          frames.properties.size(),
          config.grouping_columns.size() + config.aggregate_columns.size());
    }
    prop_columns.reserve(frames.properties.size());
    for (const SqlFrames::PropertyColumn& col : frames.properties) {
      prop_columns.push_back(InternPropColumn(col, pool));
    }
    auto find_prop = [&](const std::string& name) -> const PropColumn* {
      for (const PropColumn& p : prop_columns) {
        if (p.name == name) {
          return &p;
        }
      }
      return nullptr;
    };
    for (const std::string& name : config.grouping_columns) {
      const PropColumn* p = find_prop(name);
      if (!p) {
        return base::ErrStatus(
            "flamegraph: grouping column '%s' not found in the collected "
            "frames",
            name.c_str());
      }
      grouping_props.push_back(p);
    }
    for (const auto& [name, mode] : config.aggregate_columns) {
      const PropColumn* p = find_prop(name);
      if (!p) {
        return base::ErrStatus(
            "flamegraph: aggregate column '%s' not found in the collected "
            "frames",
            name.c_str());
      }
      agg_props.push_back(p);
    }
  }

  // Convert the collected frames into a Forest: parent ids resolve to
  // indices (unknown parents leave the frame unreachable, dropping it,
  // like the SQL surface always did), and the merge key is the dense id
  // of the (name, grouping values) tuple. Each distinct tuple is one name
  // dictionary entry whose extra match strings are the grouping values as
  // text, so filters see them like the frame name.
  flamegraph::Forest forest;
  forest.metric_count = 1;
  forest.parent = FlexVector<uint32_t>::CreateWithSize(n);
  forest.key = FlexVector<uint32_t>::CreateWithSize(n);
  forest.name = FlexVector<uint32_t>::CreateWithSize(n);
  forest.metrics = FlexVector<double>::CreateWithSize(n);
  if (n != 0) {
    memcpy(forest.metrics.data(), frames.values.data(), n * sizeof(double));
  }
  base::FlatHashMap<int64_t, uint32_t> id_to_idx;
  for (uint32_t i = 0; i < n; ++i) {
    id_to_idx.Insert(frames.ids[i], i);
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (frames.parent_ids[i] == SqlFrames::kNullParent) {
      forest.parent[i] = flamegraph::kNoParent;
    } else if (const uint32_t* p = id_to_idx.Find(frames.parent_ids[i]); p) {
      forest.parent[i] = *p;
    } else {
      forest.parent[i] = n;  // Unreachable: dropped by the library.
    }
  }
  TupleInterner interner(grouping_props.size());
  base::FlatHashMap<uint64_t, uint32_t> entry_of_tuple;
  bool has_grouping = !grouping_props.empty();
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t tuple = interner.Root(frames.names[i].raw_id());
    for (size_t g = 0; g < grouping_props.size(); ++g) {
      tuple = interner.Extend(g, tuple, grouping_props[g]->values[i]);
    }
    auto [entry, inserted] = entry_of_tuple.Insert(
        tuple, static_cast<uint32_t>(forest.name_table.size()));
    if (inserted) {
      forest.name_table.push_back(frames.names[i]);
      if (has_grouping) {
        forest.match_offset.push_back(
            static_cast<uint32_t>(forest.match_strings.size()));
        for (const PropColumn* prop : grouping_props) {
          // Mirrors the IFNULL(col, '') the SQL pipeline matched with.
          forest.match_strings.push_back(pool->InternString(
              base::StringView(prop->values[i].ToText(*pool))));
        }
      }
    }
    forest.name[i] = *entry;
    forest.key[i] = *entry;
  }
  if (has_grouping) {
    forest.match_offset.push_back(
        static_cast<uint32_t>(forest.match_strings.size()));
  }

  ASSIGN_OR_RETURN(flamegraph::Flamegraph fg,
                   flamegraph::Build(forest, config.core, *pool));

  // Emit the flat output: the fixed columns, then the grouping columns
  // (all merged frames share the value) and the aggregate columns (the
  // merged frames' values combined per mode, computed from each node's
  // constituents).
  //
  // Rows come out in render order: depth first, widest sibling first,
  // with ids equal to row numbers. Together with matchedSelfValue (the
  // node's own contribution to its cumulative value) and its ancestor
  // sum, that makes x layout downstream a single window function: a
  // node's xStart is the prefix sum of matchedSelfValue over the
  // preceding rows of its tree, minus its ancestors' share. See the
  // _flamegraph_layout macro in the graphs.flamegraph stdlib module.
  struct RenderOrder {
    std::vector<uint32_t> order;   // Preorder, widest sibling first.
    std::vector<uint32_t> row_of;  // Node -> row within the tree.
    std::vector<double> matched_self;
    std::vector<double> ancestor_matched;
    std::vector<int64_t> depth;  // By node.
  };
  auto render_order = [](const flamegraph::Tree& tree) {
    RenderOrder r;
    auto nodes = static_cast<uint32_t>(tree.size());
    r.matched_self.resize(nodes);
    r.ancestor_matched.resize(nodes);
    r.depth.resize(nodes);
    std::vector<uint32_t> child_offset(nodes + 2, 0);
    for (uint32_t i = 0; i < nodes; ++i) {
      r.matched_self[i] = tree.cumulative[i];
      if (tree.parent[i] != flamegraph::kNoParent) {
        child_offset[tree.parent[i] + 2]++;
      }
    }
    for (uint32_t i = 2; i < nodes + 2; ++i) {
      child_offset[i] += child_offset[i - 1];
    }
    std::vector<uint32_t> child_list(child_offset[nodes + 1]);
    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < nodes; ++i) {
      uint32_t p = tree.parent[i];
      if (p == flamegraph::kNoParent) {
        roots.push_back(i);
        r.depth[i] = 1;
        r.ancestor_matched[i] = 0;
      } else {
        child_list[child_offset[p + 1]++] = i;
        r.matched_self[p] -= tree.cumulative[i];
        r.depth[i] = r.depth[p] + 1;
      }
    }
    // Ancestor sums need the parents' final matched_self, so a second
    // pass (parents always precede children in node order).
    for (uint32_t i = 0; i < nodes; ++i) {
      uint32_t p = tree.parent[i];
      if (p != flamegraph::kNoParent) {
        r.ancestor_matched[i] = r.ancestor_matched[p] + r.matched_self[p];
      }
    }
    auto widest_first = [&](uint32_t a, uint32_t b) {
      if (tree.cumulative[a] > tree.cumulative[b]) {
        return true;
      }
      if (tree.cumulative[b] > tree.cumulative[a]) {
        return false;
      }
      return a < b;
    };
    std::sort(roots.begin(), roots.end(), widest_first);
    for (uint32_t i = 0; i < nodes; ++i) {
      std::sort(child_list.begin() + child_offset[i],
                child_list.begin() + child_offset[i + 1], widest_first);
    }
    r.row_of.resize(nodes);
    r.order.reserve(nodes);
    std::vector<uint32_t> stack(roots.rbegin(), roots.rend());
    while (!stack.empty()) {
      uint32_t i = stack.back();
      stack.pop_back();
      r.row_of[i] = static_cast<uint32_t>(r.order.size());
      r.order.push_back(i);
      for (uint32_t c = child_offset[i + 1]; c-- > child_offset[i];) {
        stack.push_back(child_list[c]);
      }
    }
    return r;
  };

  std::vector<std::string> columns = {
      "id",         "parentId",        "depth",
      "name",       "selfValue",       "cumulativeValue",
      "parentCumulativeValue",         "matchedSelfValue",
      "ancestorMatchedSelfValue"};
  for (const std::string& name : config.grouping_columns) {
    columns.push_back(name);
  }
  for (const auto& [name, mode] : config.aggregate_columns) {
    columns.push_back(name);
  }
  // Dense null storage: the output is read back cell by cell through
  // SQLite, where sparse-null popcount lookups dominate the scan cost.
  AdhocDataframeBuilder builder(
      columns, pool,
      AdhocDataframeBuilder::Options{{}, NullabilityType::kDenseNull});
  StringPool::Id unknown = pool->InternString("unknown");
  std::vector<PropValue> seen;
  bool ok = true;
  int64_t base_id = 0;
  for (const flamegraph::Tree* tree : {&fg.down, &fg.up}) {
    bool up = tree == &fg.up;
    RenderOrder ro = render_order(*tree);
    for (uint32_t row = 0; row < tree->size() && ok; ++row) {
      uint32_t i = ro.order[row];
      bool root = tree->parent[i] == flamegraph::kNoParent;
      uint32_t rep = tree->rep_frame[i];
      StringPool::Id name = frames.names[rep];
      if (pool->Get(name).empty()) {
        name = unknown;
      }
      uint32_t col = 0;
      ok = ok && builder.PushNonNull(col++, base_id + row);
      ok = ok &&
           builder.PushNonNull(
               col++,
               root ? int64_t(-1) : base_id + ro.row_of[tree->parent[i]]);
      ok = ok &&
           builder.PushNonNull(col++, up ? -ro.depth[i] : ro.depth[i]);
      ok = ok && builder.PushNonNull(col++, name);
      auto push_value = [&](double v) {
        if (frames.all_values_integral) {
          ok = ok && builder.PushNonNull(col++, static_cast<int64_t>(v));
        } else {
          ok = ok && builder.PushNonNull(col++, v);
        }
      };
      push_value(tree->self[i]);
      push_value(tree->cumulative[i]);
      if (root) {
        builder.PushNull(col++);
      } else {
        push_value(tree->cumulative[tree->parent[i]]);
      }
      push_value(ro.matched_self[i]);
      push_value(ro.ancestor_matched[i]);
      for (const PropColumn* prop : grouping_props) {
        const PropValue& v = prop->values[rep];
        switch (v.kind) {
          case PropValue::Kind::kNull:
            builder.PushNull(col);
            break;
          case PropValue::Kind::kInt:
            ok = ok && builder.PushNonNull(col, v.i);
            break;
          case PropValue::Kind::kDouble:
            ok = ok && builder.PushNonNull(col, v.d);
            break;
          case PropValue::Kind::kString:
            ok = ok && builder.PushNonNull(col, v.string_id());
            break;
        }
        ++col;
      }
      uint32_t cons_begin = tree->constituents_offset[i];
      uint32_t cons_end = tree->constituents_offset[i + 1];
      for (uint32_t a = 0; a < agg_props.size(); ++a) {
        const PropColumn& prop = *agg_props[a];
        switch (config.aggregate_columns[a].second) {
          case AggMode::kOneOrSummary: {
            // The first value merged into the node, with a summary suffix
            // when there were several distinct ones. The double space and
            // the count including the shown value mirror the SQL macro
            // pipeline this replaced.
            const PropValue& first = prop.values[rep];
            if (first.kind == PropValue::Kind::kNull) {
              builder.PushNull(col);
              break;
            }
            seen.clear();
            for (uint32_t c = cons_begin; c < cons_end; ++c) {
              const PropValue& v = prop.values[tree->constituents[c]];
              if (v.kind == PropValue::Kind::kNull) {
                continue;
              }
              if (std::find(seen.begin(), seen.end(), v) == seen.end()) {
                seen.push_back(v);
              }
            }
            std::string text = first.ToText(*pool);
            if (seen.size() > 1) {
              text += "  and " + std::to_string(seen.size()) + " others";
            }
            ok = ok && builder.PushNonNull(
                           col, pool->InternString(base::StringView(text)));
            break;
          }
          case AggMode::kSum: {
            double sum = 0;
            bool has_value = false;
            bool is_double = false;
            for (uint32_t c = cons_begin; c < cons_end; ++c) {
              const PropValue& v = prop.values[tree->constituents[c]];
              if (v.kind == PropValue::Kind::kNull) {
                continue;
              }
              has_value = true;
              if (v.kind == PropValue::Kind::kDouble) {
                is_double = true;
                sum += v.d;
              } else if (v.kind == PropValue::Kind::kInt) {
                sum += static_cast<double>(v.i);
              }
              // Non-numeric values sum as 0, like SQLite's SUM.
            }
            if (!has_value) {
              builder.PushNull(col);
            } else if (is_double) {
              ok = ok && builder.PushNonNull(col, sum);
            } else {
              ok = ok &&
                   builder.PushNonNull(col, static_cast<int64_t>(sum));
            }
            break;
          }
          case AggMode::kConcatWithComma: {
            std::string concat;
            bool has_value = false;
            for (uint32_t c = cons_begin; c < cons_end; ++c) {
              const PropValue& v = prop.values[tree->constituents[c]];
              if (v.kind == PropValue::Kind::kNull) {
                continue;
              }
              if (has_value) {
                concat += ",";
              }
              has_value = true;
              concat += v.ToText(*pool);
            }
            if (!has_value) {
              builder.PushNull(col);
            } else {
              ok = ok && builder.PushNonNull(
                             col,
                             pool->InternString(base::StringView(concat)));
            }
            break;
          }
        }
        ++col;
      }
    }
    base_id += static_cast<int64_t>(tree->size());
  }
  if (!ok) {
    return builder.status();
  }
  ASSIGN_OR_RETURN(core::dataframe::Dataframe df,
                   std::move(builder).Build());
  df.Finalize();
  return df;
}

}  // namespace

void FlamegraphAgg::Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc < 4 || (argc - 4) % 2 != 0) {
    return sqlite::result::Error(
        ctx,
        "__intrinsic_flamegraph_agg: expected (id, parentId, name, value) "
        "followed by (property name, value) pairs");
  }
  auto& agg_ctx = AggCtx::GetOrCreateContextForStep(ctx);
  SqlFrames& frames = agg_ctx.frames;
  if (!agg_ctx.initialized) {
    agg_ctx.initialized = true;
    for (int i = 4; i < argc; i += 2) {
      if (sqlite::value::Type(argv[i]) != sqlite::Type::kText) {
        return sqlite::result::Error(
            ctx,
            "__intrinsic_flamegraph_agg: property names must be strings");
      }
      frames.properties.push_back({sqlite::value::Text(argv[i]), {}});
    }
  }

  if (sqlite::value::Type(argv[0]) != sqlite::Type::kInteger) {
    return sqlite::result::Error(
        ctx, "__intrinsic_flamegraph_agg: id must be an integer");
  }
  frames.ids.push_back(sqlite::value::Int64(argv[0]));
  frames.parent_ids.push_back(sqlite::value::IsNull(argv[1])
                                  ? SqlFrames::kNullParent
                                  : sqlite::value::Int64(argv[1]));

  const char* name = sqlite::value::Text(argv[2]);
  frames.names.push_back(
      GetUserData(ctx)->InternString(base::StringView(name ? name : "")));

  switch (sqlite::value::Type(argv[3])) {
    case sqlite::Type::kNull:
      frames.values.push_back(0);
      break;
    case sqlite::Type::kInteger:
      frames.values.push_back(
          static_cast<double>(sqlite::value::Int64(argv[3])));
      break;
    case sqlite::Type::kFloat:
      frames.values.push_back(sqlite::value::Double(argv[3]));
      frames.all_values_integral = false;
      break;
    case sqlite::Type::kText:
    case sqlite::Type::kBlob:
      return sqlite::result::Error(
          ctx, "__intrinsic_flamegraph_agg: value must be numeric");
  }

  for (int i = 5; i < argc; i += 2) {
    auto value = ToValue(argv[i]);
    if (!value.ok()) {
      return sqlite::utils::SetError(ctx, value.status());
    }
    frames.properties[static_cast<size_t>((i - 5) / 2)].values.push_back(
        std::move(*value));
  }
}

void FlamegraphAgg::Final(sqlite3_context* ctx) {
  auto raw_agg_ctx = AggCtx::GetContextOrNullForFinal(ctx);
  if (!raw_agg_ctx) {
    return sqlite::result::Null(ctx);
  }
  return sqlite::result::UniquePointer(
      ctx,
      std::make_unique<SqlFrames>(std::move(raw_agg_ctx.get()->frames)),
      kFramesTag);
}

void FlamegraphConfig::Step(sqlite3_context* ctx,
                            int argc,
                            sqlite3_value** argv) {
  SQLITE_ASSIGN_OR_RETURN(ctx, SqlConfig config, ParseConfig(argc, argv));
  return sqlite::result::UniquePointer(
      ctx, std::make_unique<SqlConfig>(std::move(config)), kConfigTag);
}

void FlamegraphBuild::Step(sqlite3_context* ctx,
                           int argc,
                           sqlite3_value** argv) {
  PERFETTO_DCHECK(argc == kArgCount);
  base::ignore_result(argc);

  const auto* config = sqlite::value::Pointer<SqlConfig>(argv[1], kConfigTag);
  if (!config) {
    return sqlite::result::Error(
        ctx,
        "__intrinsic_flamegraph: config must be built with "
        "__intrinsic_flamegraph_config");
  }
  // A null frames pointer means the aggregation ran over zero rows: the
  // computation returns an empty table with the schema derived from the
  // config.
  const auto* frames =
      sqlite::value::Pointer<SqlFrames>(argv[0], kFramesTag);
  SqlFrames empty;
  SQLITE_ASSIGN_OR_RETURN(
      ctx, auto df,
      ComputeFlamegraph(frames ? *frames : empty, *config,
                        GetUserData(ctx)));
  return sqlite::result::UniquePointer(
      ctx, std::make_unique<dataframe::Dataframe>(std::move(df)), "TABLE");
}

namespace flamegraph {
namespace {

class FlamegraphPlugin : public Plugin<FlamegraphPlugin> {
 public:
  ~FlamegraphPlugin() override;

  void RegisterFunctions(PerfettoSqlConnection*,
                         std::vector<FunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    out.push_back(MakeFunctionRegistration<FlamegraphConfig>(nullptr));
    out.push_back(MakeFunctionRegistration<FlamegraphBuild>(pool));
  }

  void RegisterAggregateFunctions(
      PerfettoSqlConnection*,
      std::vector<AggregateFunctionRegistration>& out) override {
    StringPool* pool = trace_context_->storage->mutable_string_pool();
    out.push_back(MakeAggregateRegistration<FlamegraphAgg>(pool));
  }
};

FlamegraphPlugin::~FlamegraphPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<FlamegraphPlugin>();
      },
      FlamegraphPlugin::kPluginId, FlamegraphPlugin::kDepIds.data(),
      FlamegraphPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace flamegraph
}  // namespace perfetto::trace_processor
