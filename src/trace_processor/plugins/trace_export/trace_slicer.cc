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

#include "src/trace_processor/plugins/trace_export/trace_slicer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/sort_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::trace_export {
namespace {

using core::BitVector;
using core::FlexVector;
using core::dataframe::Column;
using core::dataframe::ColumnSpec;
using core::dataframe::Dataframe;
using core::dataframe::DataframeSpec;
using core::dataframe::Nullability;
using core::dataframe::NullStorage;
using core::dataframe::SortState;
using core::dataframe::Storage;
using core::dataframe::StorageType;

constexpr uint32_t kInvalidId = std::numeric_limits<uint32_t>::max();

struct SetIdRelation {
  std::string_view set_table;
  std::string_view set_col;
  std::vector<std::string_view> ref_cols;
};

bool IsNullable(Nullability nullability) {
  return !nullability.Is<core::NonNull>();
}

bool IsSparse(Nullability nullability) {
  return nullability.Is<core::SparseNull>() ||
         nullability.Is<core::SparseNullWithPopcountAlways>() ||
         nullability.Is<core::SparseNullWithPopcountUntilFinalization>();
}

bool NeedsPopcount(Nullability nullability) {
  return nullability.Is<core::SparseNullWithPopcountAlways>() ||
         nullability.Is<core::SparseNullWithPopcountUntilFinalization>();
}

std::optional<uint32_t> FindColumn(const Dataframe& df, std::string_view name) {
  const auto& names = df.column_names();
  for (uint32_t i = 0; i < names.size(); ++i) {
    if (names[i] == name) {
      return i;
    }
  }
  return std::nullopt;
}

template <typename T>
using ValueTypeForTag = std::remove_cv_t<std::remove_reference_t<
    decltype(std::declval<Storage>().unchecked_get<T>()[0])>>;

template <typename T>
std::optional<ValueTypeForTag<T>> ReadCell(const Column& col,
                                           uint32_t row,
                                           uint32_t* sparse_idx) {
  Nullability nullability = col.null_storage.nullability();
  const auto& data = col.storage.unchecked_get<T>();
  if (nullability.Is<core::NonNull>()) {
    return data[row];
  }
  if (nullability.Is<core::DenseNull>()) {
    if (!col.null_storage.unchecked_get<core::DenseNull>().bit_vector.is_set(
            row)) {
      return std::nullopt;
    }
    return data[row];
  }
  const auto& nulls = col.null_storage.unchecked_get<core::SparseNull>();
  if (!nulls.bit_vector.is_set(row)) {
    return std::nullopt;
  }
  ValueTypeForTag<T> val = data[*sparse_idx];
  ++(*sparse_idx);
  return val;
}

template <typename T>
void AppendCell(Column* dst, std::optional<ValueTypeForTag<T>> val) {
  Nullability nullability = dst->null_storage.nullability();
  auto& data = dst->storage.unchecked_get<T>();
  if (nullability.Is<core::NonNull>()) {
    PERFETTO_DCHECK(val.has_value());
    data.push_back(*val);
    return;
  }
  if (nullability.Is<core::DenseNull>()) {
    auto& bv = dst->null_storage.unchecked_get<core::DenseNull>().bit_vector;
    if (val.has_value()) {
      bv.push_back(true);
      data.push_back(*val);
    } else {
      bv.push_back(false);
      data.push_back(ValueTypeForTag<T>{});
    }
    return;
  }
  auto& bv = dst->null_storage.unchecked_get<core::SparseNull>().bit_vector;
  if (val.has_value()) {
    bv.push_back(true);
    data.push_back(*val);
  } else {
    bv.push_back(false);
  }
}

void FinalizePopcounts(std::vector<std::shared_ptr<Column>>& columns) {
  for (auto& col : columns) {
    Nullability nullability = col->null_storage.nullability();
    if (IsSparse(nullability) && NeedsPopcount(nullability)) {
      auto& nulls = col->null_storage.unchecked_get<core::SparseNull>();
      nulls.prefix_popcount_for_cell_get =
          nulls.bit_vector.PrefixPopcountFlexVector();
    }
  }
}

template <typename T>
void CopyColumnFiltered(const Column& src,
                        const std::vector<uint32_t>& row_remap,
                        Column* dst) {
  uint32_t sparse_idx = 0;
  for (uint32_t row = 0; row < row_remap.size(); ++row) {
    std::optional<ValueTypeForTag<T>> val = ReadCell<T>(src, row, &sparse_idx);
    if (row_remap[row] != kInvalidId) {
      AppendCell<T>(dst, val);
    }
  }
}

}  // namespace

base::StatusOr<SlicedDataframes> TraceSlicer::Slice(
    const std::vector<PluginDataframe>& dataframes,
    const TraceProcessor::ExportOptions& options) {
  int64_t start_ts = options.start_ts.value_or(0);
  int64_t end_ts = options.end_ts.value_or(std::numeric_limits<int64_t>::max());
  if (start_ts < 0) {
    return base::ErrStatus("export: start_ts must be non-negative");
  }
  if (end_ts < 0) {
    return base::ErrStatus("export: end_ts must be non-negative");
  }
  if (start_ts > end_ts) {
    return base::ErrStatus("export: start_ts (%" PRId64
                           ") must be <= end_ts (%" PRId64 ")",
                           start_ts, end_ts);
  }

  size_t num_tables = dataframes.size();
  base::FlatHashMap<std::string, size_t> name_to_idx;
  for (size_t i = 0; i < num_tables; ++i) {
    name_to_idx.Insert(dataframes[i].name, i);
  }

  struct TableSliceState {
    bool filtered = false;
    uint32_t new_row_count = 0;
    std::vector<uint32_t> row_remap;
    // Per-column overrides for old rows (indexed by old row_id).
    base::FlatHashMap<uint32_t, std::vector<int64_t>> int64_overrides;
    base::FlatHashMap<uint32_t, std::vector<std::optional<uint32_t>>>
        uint32_overrides;
  };

  std::vector<TableSliceState> states(num_tables);
  for (size_t i = 0; i < num_tables; ++i) {
    uint32_t rows = dataframes[i].dataframe->row_count();
    states[i].new_row_count = rows;
  }

  // Pass 1: Filter time-based tables and record interval/counter overrides.
  for (size_t i = 0; i < num_tables; ++i) {
    const PluginDataframe& pdf = dataframes[i];
    const Dataframe& df = *pdf.dataframe;
    uint32_t rows = df.row_count();
    if (rows == 0) {
      continue;
    }

    // Identify primary timestamp column if any.
    std::optional<uint32_t> ts_col = FindColumn(df, "ts");
    if (!ts_col) {
      ts_col = FindColumn(df, "timestamp");
    }
    if (!ts_col) {
      ts_col = FindColumn(df, "graph_sample_ts");
    }
    if (!ts_col || !df.column(*ts_col).storage.type().Is<core::Int64>()) {
      continue;
    }

    std::optional<uint32_t> dur_col = FindColumn(df, "dur");
    if (dur_col && !df.column(*dur_col).storage.type().Is<core::Int64>()) {
      dur_col = std::nullopt;
    }
    std::optional<uint32_t> ts_end_col = FindColumn(df, "ts_end");
    if (ts_end_col &&
        !df.column(*ts_end_col).storage.type().Is<core::Int64>()) {
      ts_end_col = std::nullopt;
    }

    // Check if this is a counter table: has "ts", no "dur", "track_id" FK to
    // "__intrinsic_track", and "value" Double.
    std::optional<uint32_t> track_id_col = FindColumn(df, "track_id");
    std::optional<uint32_t> value_col = FindColumn(df, "value");
    bool is_counter =
        !dur_col && track_id_col && value_col &&
        df.column(*track_id_col).storage.type().Is<core::Uint32>() &&
        df.column(*value_col).storage.type().Is<core::Double>() &&
        *track_id_col < pdf.foreign_key_tables.size() &&
        pdf.foreign_key_tables[*track_id_col] != nullptr &&
        std::string_view(pdf.foreign_key_tables[*track_id_col]) ==
            "__intrinsic_track";

    std::vector<uint8_t> keep(rows, 0);

    if (ts_end_col) {
      // HeapProfileTable (__intrinsic_heap_profile): filter by ts_end in
      // [start_ts, end_ts] without clamping ts_end so joins with
      // heap_profile_allocation.ts remain intact.
      const Column& col_ts_end = df.column(*ts_end_col);
      uint32_t sparse_idx = 0;
      for (uint32_t r = 0; r < rows; ++r) {
        auto val = ReadCell<core::Int64>(col_ts_end, r, &sparse_idx);
        if (!val.has_value() || (*val >= start_ts && *val <= end_ts)) {
          keep[r] = 1;
        }
      }
    } else if (dur_col) {
      // Interval table (ts, dur).
      const Column& col_ts = df.column(*ts_col);
      const Column& col_dur = df.column(*dur_col);
      std::vector<int64_t> new_ts_vals(rows, 0);
      std::vector<int64_t> new_dur_vals(rows, 0);
      uint32_t ts_sparse = 0;
      uint32_t dur_sparse = 0;
      bool any_clamped = false;
      for (uint32_t r = 0; r < rows; ++r) {
        auto ts_opt = ReadCell<core::Int64>(col_ts, r, &ts_sparse);
        auto dur_opt = ReadCell<core::Int64>(col_dur, r, &dur_sparse);
        if (!ts_opt.has_value() || !dur_opt.has_value()) {
          keep[r] = 1;
          new_ts_vals[r] = ts_opt.value_or(0);
          new_dur_vals[r] = dur_opt.value_or(0);
          continue;
        }
        int64_t ts = *ts_opt;
        int64_t dur = *dur_opt;
        if (dur == 0) {
          if (ts >= start_ts && ts <= end_ts) {
            keep[r] = 1;
            new_ts_vals[r] = ts;
            new_dur_vals[r] = 0;
          }
        } else if (dur < 0) {
          // Unclosed slice/state (dur == -1).
          if (ts <= end_ts) {
            keep[r] = 1;
            int64_t clamped_ts = std::max(ts, start_ts);
            new_ts_vals[r] = clamped_ts;
            new_dur_vals[r] = dur;
            if (clamped_ts != ts) {
              any_clamped = true;
            }
          }
        } else {
          int64_t slice_end = ts + dur;
          bool intersects = (start_ts == end_ts)
                                ? (ts <= end_ts && slice_end >= start_ts)
                                : (ts < end_ts && slice_end > start_ts);
          if (intersects) {
            keep[r] = 1;
            int64_t clamped_ts = std::max(ts, start_ts);
            int64_t clamped_end = std::min(slice_end, end_ts);
            int64_t clamped_dur =
                std::max<int64_t>(0, clamped_end - clamped_ts);
            new_ts_vals[r] = clamped_ts;
            new_dur_vals[r] = clamped_dur;
            if (clamped_ts != ts || clamped_dur != dur) {
              any_clamped = true;
            }
          }
        }
      }
      if (any_clamped) {
        states[i].int64_overrides.Insert(*ts_col, std::move(new_ts_vals));
        states[i].int64_overrides.Insert(*dur_col, std::move(new_dur_vals));
      }
    } else if (is_counter) {
      // Counter table: carry forward the last sample before start_ts per
      // track_id (if no sample exists at exact start_ts for that track) and
      // clamp its ts to start_ts.
      const Column& col_ts = df.column(*ts_col);
      const Column& col_track = df.column(*track_id_col);
      std::vector<int64_t> new_ts_vals(rows, 0);
      base::FlatHashMap<uint32_t, uint32_t> last_before_start;
      base::FlatHashMap<uint32_t, bool> has_at_start;
      uint32_t ts_sparse = 0;
      uint32_t track_sparse = 0;
      for (uint32_t r = 0; r < rows; ++r) {
        auto ts_opt = ReadCell<core::Int64>(col_ts, r, &ts_sparse);
        auto track_opt = ReadCell<core::Uint32>(col_track, r, &track_sparse);
        int64_t ts = ts_opt.value_or(0);
        new_ts_vals[r] = ts;
        if (ts < start_ts) {
          if (track_opt.has_value()) {
            last_before_start[*track_opt] = r;
          }
        } else if (ts <= end_ts) {
          keep[r] = 1;
          if (ts == start_ts && track_opt.has_value()) {
            has_at_start[*track_opt] = true;
          }
        }
      }
      bool any_clamped = false;
      if (end_ts >= start_ts) {
        for (auto it = last_before_start.GetIterator(); it; ++it) {
          if (!has_at_start.Find(it.key())) {
            uint32_t r = it.value();
            keep[r] = 1;
            new_ts_vals[r] = start_ts;
            any_clamped = true;
          }
        }
      }
      if (any_clamped) {
        states[i].int64_overrides.Insert(*ts_col, std::move(new_ts_vals));
      }
    } else {
      // Point-in-time table.
      const Column& col_ts = df.column(*ts_col);
      uint32_t ts_sparse = 0;
      for (uint32_t r = 0; r < rows; ++r) {
        auto ts_opt = ReadCell<core::Int64>(col_ts, r, &ts_sparse);
        if (!ts_opt.has_value() || (*ts_opt >= start_ts && *ts_opt <= end_ts)) {
          keep[r] = 1;
        }
      }
    }

    uint32_t kept_count = 0;
    std::vector<uint32_t> remap(rows, kInvalidId);
    for (uint32_t r = 0; r < rows; ++r) {
      if (keep[r]) {
        remap[r] = kept_count++;
      }
    }
    if (kept_count < rows || states[i].int64_overrides.size() > 0) {
      states[i].filtered = true;
      states[i].new_row_count = kept_count;
      states[i].row_remap = std::move(remap);
    }
  }

  // Pass 2: Propagate row drops and foreign-key ID remapping along
  // `foreign_key_tables` edges until convergence.
  //
  // Rules for a column `c` with `foreign_key_tables[c] == target_name`:
  // - If `target_table` was not filtered, no change.
  // - If `target_table` was filtered:
  //   - If `c` is NonNull and `target_remap[old_val] == kInvalidId`, drop this
  //     row!
  //   - If `c` is Nullable and `target_remap[old_val] == kInvalidId`:
  //     - If `target_table` is self (`target_idx == i`, e.g., `slice.parent_id`
  //       or `thread_state.waker_id`), set the cell to `std::nullopt` (and if
  //       `slice.parent_id` becomes null or changes, we also recompute `depth`
  //       for `__intrinsic_slice`).
  //     - If `target_table` is another table, set the cell to `std::nullopt`.
  //   - Otherwise, remap the cell value to `target_remap[old_val]`.
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < num_tables; ++i) {
      const PluginDataframe& pdf = dataframes[i];
      const Dataframe& df = *pdf.dataframe;
      uint32_t rows = df.row_count();
      if (rows == 0 || pdf.foreign_key_tables.empty()) {
        continue;
      }

      bool has_filtered_fk_target = false;
      for (uint32_t c = 0;
           c < df.column_count() && c < pdf.foreign_key_tables.size(); ++c) {
        const char* target_name = pdf.foreign_key_tables[c];
        if (!target_name) {
          continue;
        }
        auto* target_idx_ptr = name_to_idx.Find(target_name);
        if (target_idx_ptr && states[*target_idx_ptr].filtered) {
          has_filtered_fk_target = true;
          break;
        }
      }
      if (!has_filtered_fk_target) {
        continue;
      }

      // Ensure row_remap is initialized for table i.
      if (states[i].row_remap.empty()) {
        states[i].row_remap.resize(rows);
        for (uint32_t r = 0; r < rows; ++r) {
          states[i].row_remap[r] = r;
        }
      }

      std::vector<uint8_t> row_dropped(rows, 0);
      for (uint32_t r = 0; r < rows; ++r) {
        if (states[i].row_remap[r] == kInvalidId) {
          row_dropped[r] = 1;
        }
      }

      bool newly_dropped = false;
      for (uint32_t c = 0;
           c < df.column_count() && c < pdf.foreign_key_tables.size(); ++c) {
        const char* target_name = pdf.foreign_key_tables[c];
        if (!target_name) {
          continue;
        }
        auto* target_idx_ptr = name_to_idx.Find(target_name);
        if (!target_idx_ptr || !states[*target_idx_ptr].filtered) {
          continue;
        }
        const auto& target_remap = states[*target_idx_ptr].row_remap;
        const Column& col = df.column(c);
        if (!col.storage.type().Is<core::Uint32>()) {
          continue;
        }
        bool nullable = IsNullable(col.null_storage.nullability());
        std::vector<std::optional<uint32_t>> remapped_vals(rows, std::nullopt);
        uint32_t sparse_idx = 0;
        for (uint32_t r = 0; r < rows; ++r) {
          auto val_opt = ReadCell<core::Uint32>(col, r, &sparse_idx);
          if (row_dropped[r] || !val_opt.has_value()) {
            continue;
          }
          uint32_t old_target = *val_opt;
          uint32_t new_target = (old_target < target_remap.size())
                                    ? target_remap[old_target]
                                    : kInvalidId;
          if (new_target == kInvalidId) {
            if (!nullable) {
              row_dropped[r] = 1;
              newly_dropped = true;
            } else {
              remapped_vals[r] = std::nullopt;
            }
          } else {
            remapped_vals[r] = new_target;
          }
        }
        states[i].uint32_overrides[c] = std::move(remapped_vals);
        states[i].filtered = true;
      }

      if (newly_dropped) {
        uint32_t kept = 0;
        for (uint32_t r = 0; r < rows; ++r) {
          if (!row_dropped[r]) {
            states[i].row_remap[r] = kept++;
          } else {
            states[i].row_remap[r] = kInvalidId;
          }
        }
        if (kept != states[i].new_row_count) {
          states[i].new_row_count = kept;
          states[i].filtered = true;
          changed = true;
        }
      }
    }
  }

  // If `__intrinsic_slice` was filtered and has `parent_id` + `depth`, update
  // `depth` so that if a root slice was dropped, child slices have consistent
  // depth relative to surviving ancestors.
  if (auto* slice_idx_ptr = name_to_idx.Find("__intrinsic_slice")) {
    size_t slice_idx = *slice_idx_ptr;
    if (states[slice_idx].filtered) {
      const Dataframe& df = *dataframes[slice_idx].dataframe;
      auto parent_col_opt = FindColumn(df, "parent_id");
      auto depth_col_opt = FindColumn(df, "depth");
      if (parent_col_opt && depth_col_opt) {
        uint32_t rows = df.row_count();
        const auto* parent_overrides =
            states[slice_idx].uint32_overrides.Find(*parent_col_opt);
        if (parent_overrides) {
          std::vector<std::optional<uint32_t>> new_depths(rows, std::nullopt);
          // Computed depths indexed by new_row_id.
          std::vector<uint32_t> depth_by_new_id(states[slice_idx].new_row_count,
                                                0);
          for (uint32_t r = 0; r < rows; ++r) {
            uint32_t new_id = states[slice_idx].row_remap[r];
            if (new_id == kInvalidId) {
              continue;
            }
            std::optional<uint32_t> new_parent = (*parent_overrides)[r];
            uint32_t d = 0;
            if (new_parent.has_value() && *new_parent < new_id) {
              d = depth_by_new_id[*new_parent] + 1;
            }
            depth_by_new_id[new_id] = d;
            new_depths[r] = d;
          }
          states[slice_idx].uint32_overrides[*depth_col_opt] =
              std::move(new_depths);
        }
      }
    }
  }

  // Pass 3: Compact `SetIdSorted` tables whose referencing tables were filtered
  // (or which were themselves filtered via FKs, like
  // `__intrinsic_heap_graph_reference` -> `__intrinsic_heap_graph_object`).
  //
  // Recall the SetIdSorted invariant: for every contiguous group of rows with
  // set_id == V, the group must start at row index V (`storage[V] == V`).
  const SetIdRelation kSetIdRelations[] = {
      {"__intrinsic_args",
       "arg_set_id",
       {"arg_set_id", "source_arg_set_id", "dimension_arg_set_id"}},
      {"__intrinsic_profiler_counter_set",
       "counter_set_id",
       {"counter_set_id"}},
      {"__intrinsic_heap_graph_reference",
       "reference_set_id",
       {"reference_set_id"}},
  };

  for (const auto& rel : kSetIdRelations) {
    auto* set_table_idx_ptr = name_to_idx.Find(std::string(rel.set_table));
    if (!set_table_idx_ptr) {
      continue;
    }
    size_t set_table_idx = *set_table_idx_ptr;
    const Dataframe& set_df = *dataframes[set_table_idx].dataframe;
    uint32_t set_rows = set_df.row_count();
    if (set_rows == 0) {
      continue;
    }
    auto set_col_opt = FindColumn(set_df, rel.set_col);
    if (!set_col_opt) {
      continue;
    }
    uint32_t set_col_idx = *set_col_opt;
    const Column& set_col = set_df.column(set_col_idx);
    if (!set_col.storage.type().Is<core::Uint32>()) {
      continue;
    }

    // Check whether any table referencing this set was filtered, or the set
    // table itself was filtered.
    bool any_ref_filtered = states[set_table_idx].filtered;
    struct RefColLoc {
      size_t table_idx;
      uint32_t col_idx;
    };
    std::vector<RefColLoc> ref_locs;
    for (size_t t = 0; t < num_tables; ++t) {
      if (t == set_table_idx) {
        continue;
      }
      const Dataframe& df = *dataframes[t].dataframe;
      for (std::string_view ref_col_name : rel.ref_cols) {
        auto c_opt = FindColumn(df, ref_col_name);
        if (c_opt && df.column(*c_opt).storage.type().Is<core::Uint32>()) {
          ref_locs.push_back({t, *c_opt});
          if (states[t].filtered) {
            any_ref_filtered = true;
          }
        }
      }
    }
    if (!any_ref_filtered) {
      continue;
    }

    // Collect all referenced old set_ids across surviving rows of referencing
    // tables.
    std::vector<uint8_t> referenced_set(set_rows + 1, 0);
    for (const auto& loc : ref_locs) {
      const Dataframe& df = *dataframes[loc.table_idx].dataframe;
      uint32_t rows = df.row_count();
      const Column& col = df.column(loc.col_idx);
      const auto& t_state = states[loc.table_idx];
      uint32_t sparse_idx = 0;
      for (uint32_t r = 0; r < rows; ++r) {
        auto val_opt = ReadCell<core::Uint32>(col, r, &sparse_idx);
        if (t_state.filtered && t_state.row_remap[r] == kInvalidId) {
          continue;
        }
        if (val_opt.has_value() && *val_opt < referenced_set.size()) {
          referenced_set[*val_opt] = 1;
        }
      }
    }

    // Ensure row_remap is initialized on the set table.
    auto& set_state = states[set_table_idx];
    if (set_state.row_remap.empty()) {
      set_state.row_remap.resize(set_rows);
      for (uint32_t r = 0; r < set_rows; ++r) {
        set_state.row_remap[r] = r;
      }
    }

    const auto& old_set_ids = set_col.storage.unchecked_get<core::Uint32>();
    std::vector<uint32_t> set_id_remap(set_rows + 1, kInvalidId);
    std::vector<std::optional<uint32_t>> new_set_col_vals(set_rows,
                                                          std::nullopt);

    uint32_t new_row_idx = 0;
    uint32_t r = 0;
    while (r < set_rows) {
      uint32_t old_sid = old_set_ids[r];
      uint32_t group_end = r + 1;
      while (group_end < set_rows && old_set_ids[group_end] == old_sid) {
        ++group_end;
      }

      bool keep_group =
          (old_sid < referenced_set.size() && referenced_set[old_sid] != 0);

      uint32_t group_new_sid = new_row_idx;
      uint32_t surviving_in_group = 0;
      for (uint32_t k = r; k < group_end; ++k) {
        if (keep_group && set_state.row_remap[k] != kInvalidId) {
          set_state.row_remap[k] = new_row_idx++;
          new_set_col_vals[k] = group_new_sid;
          ++surviving_in_group;
        } else {
          set_state.row_remap[k] = kInvalidId;
        }
      }
      if (surviving_in_group > 0 && old_sid < set_id_remap.size()) {
        set_id_remap[old_sid] = group_new_sid;
      }
      r = group_end;
    }

    set_state.filtered = true;
    set_state.new_row_count = new_row_idx;
    set_state.uint32_overrides[set_col_idx] = std::move(new_set_col_vals);

    // Update referencing columns across all tables.
    for (const auto& loc : ref_locs) {
      const Dataframe& df = *dataframes[loc.table_idx].dataframe;
      uint32_t rows = df.row_count();
      if (rows == 0) {
        continue;
      }
      auto& t_state = states[loc.table_idx];
      if (t_state.row_remap.empty()) {
        t_state.row_remap.resize(rows);
        for (uint32_t row = 0; row < rows; ++row) {
          t_state.row_remap[row] = row;
        }
      }
      const Column& col = df.column(loc.col_idx);
      bool nullable = IsNullable(col.null_storage.nullability());
      std::vector<std::optional<uint32_t>> remapped_refs(rows, std::nullopt);
      uint32_t sparse_idx = 0;
      bool any_changed = false;
      for (uint32_t row = 0; row < rows; ++row) {
        auto val_opt = ReadCell<core::Uint32>(col, row, &sparse_idx);
        if (t_state.row_remap[row] == kInvalidId || !val_opt.has_value()) {
          continue;
        }
        uint32_t old_sid = *val_opt;
        uint32_t new_sid = (old_sid < set_id_remap.size())
                               ? set_id_remap[old_sid]
                               : kInvalidId;
        if (new_sid == kInvalidId) {
          remapped_refs[row] = nullable ? std::nullopt : std::make_optional(0u);
          any_changed = true;
        } else {
          remapped_refs[row] = new_sid;
          if (new_sid != old_sid) {
            any_changed = true;
          }
        }
      }
      if (any_changed) {
        t_state.filtered = true;
        t_state.uint32_overrides[loc.col_idx] = std::move(remapped_refs);
      }
    }
  }

  // Materialize new Dataframes for all filtered tables.
  SlicedDataframes result;
  result.owned_dataframes.reserve(num_tables);
  result.dataframes.reserve(num_tables);

  for (size_t i = 0; i < num_tables; ++i) {
    const PluginDataframe& pdf = dataframes[i];
    if (!states[i].filtered) {
      result.dataframes.push_back(pdf);
      continue;
    }

    const Dataframe& src_df = *pdf.dataframe;
    DataframeSpec spec = src_df.CreateSpec();
    auto columns = Dataframe::CreateColumnVector(
        spec.column_specs.data(),
        static_cast<uint32_t>(spec.column_specs.size()));

    uint32_t old_rows = src_df.row_count();
    uint32_t new_rows = states[i].new_row_count;
    const auto& row_remap = states[i].row_remap;

    for (uint32_t c = 0; c < src_df.column_count(); ++c) {
      const Column& src_col = src_df.column(c);
      Column* dst_col = columns[c].get();
      StorageType type = src_col.storage.type();

      if (type.Is<core::Id>()) {
        dst_col->storage.unchecked_get<core::Id>().size = new_rows;
        continue;
      }

      if (type.Is<core::Int64>()) {
        if (const auto* ov = states[i].int64_overrides.Find(c)) {
          uint32_t sparse_idx = 0;
          for (uint32_t r = 0; r < old_rows; ++r) {
            auto orig = ReadCell<core::Int64>(src_col, r, &sparse_idx);
            if (row_remap[r] != kInvalidId) {
              AppendCell<core::Int64>(
                  dst_col, orig.has_value() ? std::make_optional((*ov)[r])
                                            : std::nullopt);
            }
          }
        } else {
          CopyColumnFiltered<core::Int64>(src_col, row_remap, dst_col);
        }
      } else if (type.Is<core::Uint32>()) {
        if (const auto* ov = states[i].uint32_overrides.Find(c)) {
          for (uint32_t r = 0; r < old_rows; ++r) {
            if (row_remap[r] != kInvalidId) {
              AppendCell<core::Uint32>(dst_col, (*ov)[r]);
            }
          }
        } else {
          CopyColumnFiltered<core::Uint32>(src_col, row_remap, dst_col);
        }
      } else if (type.Is<core::Int32>()) {
        CopyColumnFiltered<core::Int32>(src_col, row_remap, dst_col);
      } else if (type.Is<core::Double>()) {
        CopyColumnFiltered<core::Double>(src_col, row_remap, dst_col);
      } else if (type.Is<core::String>()) {
        CopyColumnFiltered<core::String>(src_col, row_remap, dst_col);
      }
    }

    FinalizePopcounts(columns);
    Dataframe sliced_df(/*finalized=*/true, spec.column_names,
                        std::move(columns), new_rows, src_df.string_pool_);
    result.owned_dataframes.push_back(std::move(sliced_df));
    PluginDataframe sliced_pdf = pdf;
    sliced_pdf.dataframe = &result.owned_dataframes.back();
    result.dataframes.push_back(std::move(sliced_pdf));
  }

  return std::move(result);
}

}  // namespace perfetto::trace_processor::trace_export
