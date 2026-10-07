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

#include "src/trace_processor/plugins/scatter_mipmap_operator/scatter_mipmap_index.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"

namespace perfetto::trace_processor::scatter_mipmap_operator {
namespace {

constexpr int64_t kMaxCells = 16'000'000;

inline float SafeDoubleToFloat(double d) {
  if (!std::isfinite(d)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  constexpr double kMaxFloat =
      static_cast<double>(std::numeric_limits<float>::max());
  if (d > kMaxFloat) {
    return std::numeric_limits<float>::max();
  }
  if (d < -kMaxFloat) {
    return -std::numeric_limits<float>::max();
  }
  return static_cast<float>(d);
}

void AppendOtherAndNull(std::vector<CategoryEntry>& categories,
                        uint32_t target_n,
                        uint64_t other_count,
                        uint32_t null_count) {
  if (other_count > 0) {
    CategoryEntry entry;
    entry.idx = static_cast<int32_t>(target_n);
    entry.count = other_count;
    entry.is_other = true;
    entry.str_val = "Other";
    categories.push_back(std::move(entry));
  }
  if (null_count > 0) {
    CategoryEntry entry;
    entry.idx = -1;
    entry.count = null_count;
    entry.is_null = true;
    categories.push_back(std::move(entry));
  }
}

}  // namespace

void CategoryMapping::PopulateTopN(
    const base::FlatHashMap<uint32_t, uint32_t>& str_counts,
    const base::FlatHashMap<uint64_t, uint32_t>& num_counts,
    uint32_t null_count,
    uint64_t other_mismatched_count,
    StringPool* pool) {
  uint32_t target_n = auto_top_n;
  categories.clear();
  str_pool_id_to_idx.Clear();
  num_to_idx.Clear();
  uint64_t other_count = other_mismatched_count;

  if (type == Type::kString) {
    struct Cand {
      uint32_t pool_id;
      uint32_t count;
      base::StringView str;
    };
    std::vector<Cand> cands;
    cands.reserve(str_counts.size());
    for (auto it = str_counts.GetIterator(); it; ++it) {
      base::StringView sv = (pool && it.key() != 0)
                                ? pool->Get(StringPool::Id::Raw(it.key()))
                                : base::StringView();
      cands.push_back({it.key(), it.value(), sv});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
      return a.count != b.count ? a.count > b.count : a.str < b.str;
    });
    size_t top_count = std::min(static_cast<size_t>(target_n), cands.size());
    for (size_t i = 0; i < top_count; ++i) {
      int32_t idx = static_cast<int32_t>(i);
      str_pool_id_to_idx.Insert(cands[i].pool_id, idx);
      CategoryEntry entry;
      entry.idx = idx;
      entry.count = cands[i].count;
      entry.str_val = std::string(cands[i].str.data(), cands[i].str.size());
      categories.push_back(std::move(entry));
    }
    for (size_t i = top_count; i < cands.size(); ++i) {
      other_count += cands[i].count;
    }
  } else {
    struct NumCand {
      uint64_t key;
      int64_t iv;
      double dv;
      uint32_t count;
    };
    std::vector<NumCand> cands;
    cands.reserve(num_counts.size());
    bool is_int = (type == Type::kInt);
    for (auto it = num_counts.GetIterator(); it; ++it) {
      cands.push_back({it.key(), static_cast<int64_t>(it.key()),
                       is_int ? 0.0 : KeyToDouble(it.key()), it.value()});
    }
    std::sort(cands.begin(), cands.end(),
              [is_int](const NumCand& a, const NumCand& b) {
                if (a.count != b.count)
                  return a.count > b.count;
                return is_int ? (a.iv < b.iv) : (a.dv < b.dv);
              });
    size_t top_count = std::min(static_cast<size_t>(target_n), cands.size());
    for (size_t i = 0; i < top_count; ++i) {
      int32_t idx = static_cast<int32_t>(i);
      num_to_idx.Insert(cands[i].key, idx);
      CategoryEntry entry;
      entry.idx = idx;
      entry.count = cands[i].count;
      entry.int_val = cands[i].iv;
      entry.double_val = cands[i].dv;
      categories.push_back(std::move(entry));
    }
    for (size_t i = top_count; i < cands.size(); ++i) {
      other_count += cands[i].count;
    }
  }
  AppendOtherAndNull(categories, target_n, other_count, null_count);
}

template <typename PointAccessor>
void ScatterMipmapIndex::BuildCore(uint32_t n,
                                   double x_min,
                                   double x_max,
                                   double y_min,
                                   double y_max,
                                   double c_min,
                                   double c_max,
                                   bool has_c,
                                   int64_t min_id,
                                   int64_t max_id,
                                   PointAccessor& accessor) {
  if (n == 0) {
    x_min_data_ = x_max_data_ = y_min_data_ = y_max_data_ = 0.0;
    c_min_data_ = c_max_data_ = 0.0;
    has_c_ = false;
    level_t_ = 8;
    level_t_offsets_.assign(65537, 0);
    return;
  }

  x_min_data_ = x_min;
  x_max_data_ = x_max;
  y_min_data_ = y_min;
  y_max_data_ = y_max;
  c_min_data_ = c_min;
  c_max_data_ = c_max;
  has_c_ = has_c;
  min_id_ = min_id;
  max_id_ = max_id;

  constexpr uint32_t kMaxQuant = (1u << 20) - 1;
  double x_span = x_max_data_ - x_min_data_;
  double y_span = y_max_data_ - y_min_data_;
  double inv_x_span =
      (x_span > 0.0) ? (static_cast<double>(1u << 20) / x_span) : 0.0;
  double inv_y_span =
      (y_span > 0.0) ? (static_cast<double>(1u << 20) / y_span) : 0.0;

  double log4_n = std::log(static_cast<double>(n)) / std::log(4.0);
  level_t_ = std::clamp(static_cast<uint32_t>(std::ceil(log4_n)) + 1, 8u, 11u);
  size_t table_size = (1ULL << (2 * level_t_)) + 1;
  uint32_t t_shift = 40 - 2 * level_t_;

  ids_are_32_ = (min_id_ >= 0 && max_id_ <= 0xFFFFFFFFLL);

  std::unique_ptr<uint64_t[]> pairs(new uint64_t[n]);
  std::unique_ptr<uint64_t[]> temp(new uint64_t[n]);
  uint32_t counts[2][2048] = {{0}};

  for (size_t i = 0; i < n; ++i) {
    double x = accessor.GetX(static_cast<uint32_t>(i));
    double y = accessor.GetY(static_cast<uint32_t>(i));

    uint32_t qx = 0;
    if (x_span > 0.0) {
      if (PERFETTO_UNLIKELY(x >= x_max_data_)) {
        qx = kMaxQuant;
      } else if (PERFETTO_LIKELY(x > x_min_data_)) {
        qx = std::min(kMaxQuant,
                      static_cast<uint32_t>((x - x_min_data_) * inv_x_span));
      }
    }

    uint32_t qy = 0;
    if (y_span > 0.0) {
      if (PERFETTO_UNLIKELY(y >= y_max_data_)) {
        qy = kMaxQuant;
      } else if (PERFETTO_LIKELY(y > y_min_data_)) {
        qy = std::min(kMaxQuant,
                      static_cast<uint32_t>((y - y_min_data_) * inv_y_span));
      }
    }

    uint32_t leaf_key =
        static_cast<uint32_t>(EncodeMorton2D(qx, qy) >> t_shift);
    pairs[i] =
        (static_cast<uint64_t>(leaf_key) << 32) | static_cast<uint32_t>(i);
    counts[0][leaf_key & 0x7FF]++;
    counts[1][(leaf_key >> 11) & 0x7FF]++;
  }

  uint32_t offsets[2048];
  offsets[0] = 0;
  for (size_t d = 1; d < 2048; ++d) {
    offsets[d] = offsets[d - 1] + counts[0][d - 1];
  }
  for (size_t i = 0; i < n; ++i) {
    temp[offsets[(pairs[i] >> 32) & 0x7FF]++] = pairs[i];
  }

  offsets[0] = 0;
  for (size_t d = 1; d < 2048; ++d) {
    offsets[d] = offsets[d - 1] + counts[1][d - 1];
  }
  for (size_t i = 0; i < n; ++i) {
    pairs[offsets[(temp[i] >> 43) & 0x7FF]++] = temp[i];
  }
  temp.reset();

  xs_.resize(n);
  ys_.resize(n);
  if (ids_are_32_) {
    ids32_.resize(n);
    ids64_.clear();
  } else {
    ids64_.resize(n);
    ids32_.clear();
  }
  if (has_c_) {
    cs_.resize(n);
  } else {
    cs_.clear();
  }
  level_t_offsets_.resize(table_size);
  uint32_t offset_idx = 0;

  for (size_t i = 0; i < n; ++i) {
    uint32_t orig = static_cast<uint32_t>(pairs[i]);
    uint32_t leaf_key = static_cast<uint32_t>(pairs[i] >> 32);
    xs_[i] = accessor.GetX(orig);
    ys_[i] = accessor.GetY(orig);
    int64_t pt_id = accessor.GetId(orig);
    if (ids_are_32_) {
      ids32_[i] = static_cast<uint32_t>(pt_id);
    } else {
      ids64_[i] = pt_id;
    }
    if (has_c_) {
      cs_[i] = accessor.GetC(orig);
    }
    while (offset_idx <= leaf_key) {
      level_t_offsets_[offset_idx++] = static_cast<uint32_t>(i);
    }
  }
  while (offset_idx < table_size) {
    level_t_offsets_[offset_idx++] = static_cast<uint32_t>(n);
  }
}

void ScatterMipmapIndex::IngestAndBuildDataframe(
    uint32_t n,
    const DataframeColumnReader* reader_id,
    const DataframeColumnReader& reader_x,
    const DataframeColumnReader& reader_y,
    const DataframeColumnReader* reader_c,
    CategoryMapping* categories,
    StringPool* pool) {
  base::FlatHashMap<uint32_t, uint32_t> auto_str_counts;
  base::FlatHashMap<uint64_t, uint32_t> auto_num_counts;
  uint32_t auto_null_count = 0;
  bool is_auto_cat = (categories && categories->auto_top_n > 0 && reader_c);

  double x_min = 0.0, x_max = 0.0, y_min = 0.0, y_max = 0.0;
  int64_t min_id = 0, max_id = 0;
  double c_min = 0.0, c_max = 0.0;
  bool has_c = false;
  uint32_t valid_count = 0;
  bool has_invalid = false;

  for (uint32_t i = 0; i < n; ++i) {
    double x = 0.0, y = 0.0;
    if (!reader_x.GetDouble(i, x) || !reader_y.GetDouble(i, y)) {
      has_invalid = true;
      continue;
    }

    int64_t id_val = static_cast<int64_t>(i);
    if (reader_id) {
      reader_id->GetInt64(i, id_val);
    }

    if (valid_count == 0) {
      x_min = x_max = x;
      y_min = y_max = y;
      min_id = max_id = id_val;
    } else {
      x_min = std::min(x_min, x);
      x_max = std::max(x_max, x);
      y_min = std::min(y_min, y);
      y_max = std::max(y_max, y);
      min_id = std::min(min_id, id_val);
      max_id = std::max(max_id, id_val);
    }

    if (is_auto_cat) {
      if (categories->type == CategoryMapping::Type::kString) {
        StringPool::Id s_id;
        if (reader_c->GetStringPoolId(i, s_id) && !s_id.is_null()) {
          auto_str_counts[s_id.raw_id()]++;
        } else {
          auto_null_count++;
        }
      } else if (categories->type == CategoryMapping::Type::kInt) {
        int64_t iv;
        if (reader_c->GetInt64(i, iv)) {
          auto_num_counts[static_cast<uint64_t>(iv)]++;
        } else {
          auto_null_count++;
        }
      } else {
        double dv;
        if (reader_c->GetDouble(i, dv) && std::isfinite(dv)) {
          auto_num_counts[DoubleToKey(dv)]++;
        } else {
          auto_null_count++;
        }
      }
    } else if (reader_c) {
      double c_val = 0.0;
      if (reader_c->GetDouble(i, c_val) && std::isfinite(c_val)) {
        if (!has_c) {
          c_min = c_max = c_val;
          has_c = true;
        } else {
          c_min = std::min(c_min, c_val);
          c_max = std::max(c_max, c_val);
        }
      }
    }
    valid_count++;
  }

  if (is_auto_cat && valid_count > 0) {
    categories->PopulateTopN(auto_str_counts, auto_num_counts, auto_null_count,
                             0, pool);
    has_c = true;
    c_min = (auto_null_count > 0) ? -1.0 : 0.0;
    c_max = static_cast<double>(categories->auto_top_n);
  }

  std::vector<uint32_t> valid_indices;
  if (has_invalid && valid_count > 0) {
    valid_indices.reserve(valid_count);
    for (uint32_t i = 0; i < n; ++i) {
      double x = 0.0, y = 0.0;
      if (reader_x.GetDouble(i, x) && reader_y.GetDouble(i, y)) {
        valid_indices.push_back(i);
      }
    }
  }

  struct DirectAccessor {
    const DataframeColumnReader* reader_id;
    const DataframeColumnReader& reader_x;
    const DataframeColumnReader& reader_y;
    const DataframeColumnReader* reader_c;
    const CategoryMapping* categories;

    PERFETTO_ALWAYS_INLINE double GetX(uint32_t i) const {
      double v = 0.0;
      reader_x.GetDouble(i, v);
      return v;
    }
    PERFETTO_ALWAYS_INLINE double GetY(uint32_t i) const {
      double v = 0.0;
      reader_y.GetDouble(i, v);
      return v;
    }
    PERFETTO_ALWAYS_INLINE int64_t GetId(uint32_t i) const {
      int64_t v = 0;
      return (reader_id && reader_id->GetInt64(i, v)) ? v
                                                      : static_cast<int64_t>(i);
    }
    PERFETTO_ALWAYS_INLINE float GetC(uint32_t i) const {
      if (categories) {
        if (categories->type == CategoryMapping::Type::kString) {
          StringPool::Id s_id;
          return (reader_c && reader_c->GetStringPoolId(i, s_id) &&
                  !s_id.is_null())
                     ? static_cast<float>(categories->MapString(s_id.raw_id()))
                     : -1.0f;
        }
        if (categories->type == CategoryMapping::Type::kInt) {
          int64_t iv = 0;
          return (reader_c && reader_c->GetInt64(i, iv))
                     ? static_cast<float>(categories->MapInt(iv))
                     : -1.0f;
        }
        double dv = 0.0;
        return (reader_c && reader_c->GetDouble(i, dv) && std::isfinite(dv))
                   ? static_cast<float>(categories->MapDouble(dv))
                   : -1.0f;
      }
      double v = 0.0;
      return (reader_c && reader_c->GetDouble(i, v))
                 ? SafeDoubleToFloat(v)
                 : std::numeric_limits<float>::quiet_NaN();
    }
  } direct{reader_id, reader_x, reader_y, reader_c, categories};

  if (!has_invalid) {
    BuildCore(valid_count, x_min, x_max, y_min, y_max, c_min, c_max, has_c,
              min_id, max_id, direct);
  } else {
    struct IndirectAccessor {
      const std::vector<uint32_t>& valid_indices;
      DirectAccessor& direct;
      double GetX(uint32_t i) const { return direct.GetX(valid_indices[i]); }
      double GetY(uint32_t i) const { return direct.GetY(valid_indices[i]); }
      int64_t GetId(uint32_t i) const { return direct.GetId(valid_indices[i]); }
      float GetC(uint32_t i) const { return direct.GetC(valid_indices[i]); }
    } indirect{valid_indices, direct};
    BuildCore(valid_count, x_min, x_max, y_min, y_max, c_min, c_max, has_c,
              min_id, max_id, indirect);
  }

  categorical_ = categories != nullptr;
  if (categories) {
    categories_ = categories->categories;
    categories_type_ = categories->type;
  }
  BuildCategoryMasks();
}

void ScatterMipmapIndex::IngestAndBuildSqlRows(std::vector<SqlRow>& rows,
                                               CategoryMapping* categories,
                                               StringPool* pool) {
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [](const SqlRow& r) {
                              return !std::isfinite(r.x) || !std::isfinite(r.y);
                            }),
             rows.end());

  struct SqlAccessor {
    const std::vector<SqlRow>& rows;
    double GetX(uint32_t i) const { return rows[i].x; }
    double GetY(uint32_t i) const { return rows[i].y; }
    int64_t GetId(uint32_t i) const { return rows[i].id; }
    float GetC(uint32_t i) const { return SafeDoubleToFloat(rows[i].c); }
  } acc{rows};

  if (rows.empty()) {
    BuildCore(0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false, 0, 0, acc);
    return;
  }

  uint32_t n = static_cast<uint32_t>(rows.size());
  double x_min = rows[0].x, x_max = rows[0].x;
  double y_min = rows[0].y, y_max = rows[0].y;
  int64_t min_id = rows[0].id, max_id = rows[0].id;
  double c_min = 0.0, c_max = 0.0;
  bool has_c = false;

  base::FlatHashMap<uint32_t, uint32_t> auto_str_counts;
  base::FlatHashMap<uint64_t, uint32_t> auto_num_counts;
  uint32_t auto_null_count = 0;
  uint64_t other_mismatched_count = 0;
  bool is_auto_cat = (categories && categories->auto_top_n > 0);

  for (size_t i = 0; i < n; ++i) {
    const auto& r = rows[i];
    x_min = std::min(x_min, r.x);
    x_max = std::max(x_max, r.x);
    y_min = std::min(y_min, r.y);
    y_max = std::max(y_max, r.y);
    min_id = std::min(min_id, r.id);
    max_id = std::max(max_id, r.id);

    if (is_auto_cat) {
      if (r.c_is_null) {
        auto_null_count++;
      } else if (categories->type == CategoryMapping::Type::kString &&
                 r.c_is_string) {
        auto_str_counts[r.c_str_id]++;
      } else if (categories->type == CategoryMapping::Type::kInt &&
                 r.c_is_int) {
        auto_num_counts[static_cast<uint64_t>(r.c_int)]++;
      } else if (categories->type == CategoryMapping::Type::kDouble &&
                 r.c_is_double && std::isfinite(r.c_double)) {
        auto_num_counts[DoubleToKey(r.c_double)]++;
      } else {
        other_mismatched_count++;
      }
    } else if (r.has_c && std::isfinite(r.c)) {
      if (!has_c) {
        c_min = c_max = r.c;
        has_c = true;
      } else {
        c_min = std::min(c_min, r.c);
        c_max = std::max(c_max, r.c);
      }
    }
  }

  if (is_auto_cat) {
    categories->PopulateTopN(auto_str_counts, auto_num_counts, auto_null_count,
                             other_mismatched_count, pool);
    has_c = true;
    c_min = (auto_null_count > 0) ? -1.0 : 0.0;
    c_max = static_cast<double>(categories->auto_top_n);

    for (size_t i = 0; i < n; ++i) {
      auto& r = rows[i];
      if (r.c_is_null) {
        r.c = -1.0;
      } else if (categories->type == CategoryMapping::Type::kString) {
        r.c = r.c_is_string
                  ? static_cast<double>(categories->MapString(r.c_str_id))
                  : static_cast<double>(categories->auto_top_n);
      } else if (categories->type == CategoryMapping::Type::kInt) {
        r.c = r.c_is_int ? static_cast<double>(categories->MapInt(r.c_int))
                         : static_cast<double>(categories->auto_top_n);
      } else {
        r.c = (r.c_is_double && std::isfinite(r.c_double))
                  ? static_cast<double>(categories->MapDouble(r.c_double))
                  : static_cast<double>(categories->auto_top_n);
      }
    }
  }

  BuildCore(n, x_min, x_max, y_min, y_max, c_min, c_max, has_c, min_id, max_id,
            acc);

  categorical_ = categories != nullptr;
  if (categories) {
    categories_ = categories->categories;
    categories_type_ = categories->type;
  }
  BuildCategoryMasks();
  std::vector<SqlRow>().swap(rows);
}

bool ScatterMipmapIndex::TryQueryAligned(
    double x_min,
    double x_max,
    double y_min,
    double y_max,
    int64_t cols,
    int64_t rows,
    std::vector<Result>& out_results) const {
  double x_span = x_max_data_ - x_min_data_;
  double y_span = y_max_data_ - y_min_data_;
  double span_x_query = x_max - x_min;
  double span_y_query = y_max - y_min;
  if (x_span <= 0.0 || y_span <= 0.0 || span_x_query <= 0.0 ||
      span_y_query <= 0.0) {
    return false;
  }

  double rx_d = (x_span * static_cast<double>(cols)) / span_x_query;
  double ry_d = (y_span * static_cast<double>(rows)) / span_y_query;
  if (!std::isfinite(rx_d) || !std::isfinite(ry_d) || rx_d < 0.9999 ||
      ry_d < 0.9999 || !(rx_d <= 0x1p20) || !(ry_d <= 0x1p20)) {
    return false;
  }

  uint32_t lx = static_cast<uint32_t>(std::round(std::log2(rx_d)));
  uint32_t ly = static_cast<uint32_t>(std::round(std::log2(ry_d)));
  uint32_t l_max = std::max(lx, ly);
  if (l_max > level_t_) {
    return false;
  }

  double target_rx = std::ldexp(1.0, static_cast<int>(lx));
  double target_ry = std::ldexp(1.0, static_cast<int>(ly));
  if (std::abs(rx_d - target_rx) > 1e-4 * target_rx ||
      std::abs(ry_d - target_ry) > 1e-4 * target_ry) {
    return false;
  }

  double kx0 = (x_min - x_min_data_) / (x_span / target_rx);
  double ky0 = (y_min - y_min_data_) / (y_span / target_ry);
  double rx0 = std::round(kx0);
  double ry0 = std::round(ky0);
  if (!std::isfinite(rx0) || !std::isfinite(ry0) ||
      std::abs(kx0 - rx0) > 1e-4 || std::abs(ky0 - ry0) > 1e-4 || rx0 < 0.0 ||
      ry0 < 0.0 || rx0 + static_cast<double>(cols) > target_rx ||
      ry0 + static_cast<double>(rows) > target_ry) {
    return false;
  }

  int64_t ix0 = static_cast<int64_t>(rx0);
  int64_t iy0 = static_cast<int64_t>(ry0);
  out_results.clear();
  out_results.reserve(
      std::min<size_t>(size(), static_cast<size_t>(cols * rows)));

  uint32_t shift_to_t = 2 * (level_t_ - l_max);
  uint32_t dx = (ly > lx) ? (ly - lx) : 0;
  uint32_t dy = (lx > ly) ? (lx - ly) : 0;
  uint32_t subs_x = 1u << dx;
  uint32_t subs_y = 1u << dy;

  for (int64_t r = 0; r < rows; ++r) {
    uint32_t y_base = static_cast<uint32_t>((iy0 + r) << dy);
    for (int64_t c = 0; c < cols; ++c) {
      uint32_t x_base = static_cast<uint32_t>((ix0 + c) << dx);
      uint32_t total_count = 0;
      uint32_t best_rep = 0;
      uint32_t max_sub_count = 0;
      for (uint32_t sy = 0; sy < subs_y; ++sy) {
        for (uint32_t sx = 0; sx < subs_x; ++sx) {
          uint64_t morton = EncodeMorton2D(x_base + sx, y_base + sy);
          uint32_t lo = level_t_offsets_[morton << shift_to_t];
          uint32_t hi = level_t_offsets_[(morton + 1) << shift_to_t];
          uint32_t sub_count = hi - lo;
          if (sub_count > 0) {
            total_count += sub_count;
            if (sub_count > max_sub_count) {
              max_sub_count = sub_count;
              best_rep = lo + sub_count / 2;
            }
          }
        }
      }
      if (total_count > 0) {
        out_results.push_back({best_rep, total_count});
      }
    }
  }
  return true;
}

bool ScatterMipmapIndex::Query(double x_min,
                               double x_max,
                               double y_min,
                               double y_max,
                               int64_t cols,
                               int64_t rows,
                               QueryBuffers& buffers,
                               std::vector<Result>& out_results,
                               const CategoryFilter* filter) const {
  out_results.clear();
  if (empty() || !std::isfinite(x_min) || !std::isfinite(x_max) ||
      !std::isfinite(y_min) || !std::isfinite(y_max) || x_max <= x_min ||
      y_max <= y_min || cols < 1 || rows < 1 || cols > kMaxCells ||
      rows > kMaxCells || cols > kMaxCells / rows) {
    return false;
  }
  bool filtered =
      filter != nullptr && has_c_ &&
      (filter->hide_null ||
       (categorical_ && std::find(filter->hidden.begin(), filter->hidden.end(),
                                  true) != filter->hidden.end()));
  if (!filtered) {
    if (TryQueryAligned(x_min, x_max, y_min, y_max, cols, rows, out_results)) {
      return true;
    }
    filter = nullptr;
  }
  QueryDfs(x_min, x_max, y_min, y_max, cols, rows, buffers, out_results,
           filter);
  return true;
}

uint32_t ScatterMipmapIndex::CategoryBit(uint32_t idx) const {
  float c = cs_[idx];
  if (std::isnan(c)) {
    return kNullBit;
  }
  if (!categorical_) {
    return kOverflowBit;
  }
  if (c < 0.0f) {
    return kNullBit;
  }
  return c < static_cast<float>(kOverflowBit) ? static_cast<uint32_t>(c)
                                              : kOverflowBit;
}

bool ScatterMipmapIndex::IsPointHidden(uint32_t idx,
                                       const CategoryFilter& filter) const {
  float c = cs_[idx];
  if (std::isnan(c) || (categorical_ && c < 0.0f)) {
    return filter.hide_null;
  }
  if (!categorical_) {
    return false;
  }
  auto k = static_cast<size_t>(c);
  return k < filter.hidden.size() && filter.hidden[k];
}

void ScatterMipmapIndex::BuildCategoryMasks() {
  cat_masks_.clear();
  mask_level_ = 0;
  if (!has_c_ || empty()) {
    return;
  }
  mask_level_ = std::min(level_t_, kMaxMaskLevel);
  cat_masks_.assign(MaskLevelOffset(mask_level_ + 1), 0);

  // Leaf masks at `mask_level_` from the Morton-ordered point ranges.
  const uint32_t shift = 2 * (level_t_ - mask_level_);
  const size_t leaf_count = size_t{1} << (2 * mask_level_);
  uint64_t* leaves = &cat_masks_[MaskLevelOffset(mask_level_)];
  for (size_t p = 0; p < leaf_count; ++p) {
    uint32_t lo = level_t_offsets_[p << shift];
    uint32_t hi = level_t_offsets_[(p + 1) << shift];
    uint64_t mask = 0;
    for (uint32_t i = lo; i < hi; ++i) {
      mask |= uint64_t{1} << CategoryBit(i);
    }
    leaves[p] = mask;
  }

  // Reduce upwards: a parent's mask is the OR of its four children.
  for (uint32_t level = mask_level_; level > 0; --level) {
    const uint64_t* child = &cat_masks_[MaskLevelOffset(level)];
    uint64_t* parent = &cat_masks_[MaskLevelOffset(level - 1)];
    const size_t parent_count = size_t{1} << (2 * (level - 1));
    for (size_t p = 0; p < parent_count; ++p) {
      parent[p] =
          child[4 * p] | child[4 * p + 1] | child[4 * p + 2] | child[4 * p + 3];
    }
  }
}

void ScatterMipmapIndex::QueryDfs(double x_min,
                                  double x_max,
                                  double y_min,
                                  double y_max,
                                  int64_t cols,
                                  int64_t rows,
                                  QueryBuffers& buffers,
                                  std::vector<Result>& out_results,
                                  const CategoryFilter* filter) const {
  // Bits of the categories that are hidden / possibly visible (see
  // CategoryBit()). The overflow bit is shared by several categories so it is
  // always considered possibly visible.
  uint64_t hidden_bits = 0;
  if (filter != nullptr) {
    if (categorical_) {
      for (size_t k = 0; k < filter->hidden.size(); ++k) {
        if (filter->hidden[k]) {
          hidden_bits |= uint64_t{1}
                         << std::min<size_t>(k, size_t{kOverflowBit});
        }
      }
    }
    if (filter->hide_null) {
      hidden_bits |= uint64_t{1} << kNullBit;
    }
  }
  const uint64_t visible_bits = ~hidden_bits | (uint64_t{1} << kOverflowBit);

  uint64_t total_cells_u64 =
      static_cast<uint64_t>(cols) * static_cast<uint64_t>(rows);
  size_t total_cells = static_cast<size_t>(total_cells_u64);
  bool use_dense = total_cells <= 4'000'000;

  if (use_dense) {
    if (buffers.dense_slots.size() < total_cells) {
      buffers.dense_slots.resize(total_cells);
    }
    buffers.occupied_cells.clear();
  } else {
    buffers.sparse_slots.Clear();
  }

  auto add_points = [&](uint64_t cell_idx, uint32_t count, uint32_t rep_idx) {
    if (use_dense) {
      DenseSlot& slot = buffers.dense_slots[cell_idx];
      if (slot.count == 0) {
        buffers.occupied_cells.push_back(static_cast<uint32_t>(cell_idx));
        slot.rep_index = rep_idx;
      }
      slot.count += count;
    } else if (auto* slot = buffers.sparse_slots.Find(cell_idx)) {
      slot->count += count;
    } else {
      buffers.sparse_slots.Insert(cell_idx, DenseSlot{count, rep_idx});
    }
  };

  double inv_cell_x = static_cast<double>(cols) / (x_max - x_min);
  double inv_cell_y = static_cast<double>(rows) / (y_max - y_min);
  double eps_x = 1e-9 * ((x_max - x_min) / static_cast<double>(cols));
  double eps_y = 1e-9 * ((y_max - y_min) / static_cast<double>(rows));

  auto fast_cell = [](double v, double v_min, double v_max, double inv,
                      int64_t limit) -> int64_t {
    if (PERFETTO_UNLIKELY(v >= v_max))
      return limit - 1;
    if (PERFETTO_UNLIKELY(v <= v_min))
      return 0;
    return std::clamp(static_cast<int64_t>((v - v_min) * inv), int64_t{0},
                      limit - 1);
  };

  struct DfsFrame {
    uint32_t level;
    uint64_t prefix;
    uint32_t lo;
    uint32_t hi;
    double x_min;
    double x_max;
    double y_min;
    double y_max;
    // True if the node is known to contain no hidden points.
    bool clean;
  };

  DfsFrame stack[128];
  int stack_ptr = 0;
  stack[stack_ptr++] = {
      0,
      0,
      0,
      static_cast<uint32_t>(size()),
      x_min_data_,
      x_max_data_,
      y_min_data_,
      y_max_data_,
      hidden_bits == 0,
  };

  while (stack_ptr > 0) {
    DfsFrame curr = stack[--stack_ptr];
    if (curr.lo >= curr.hi || curr.x_max < x_min || curr.x_min > x_max ||
        curr.y_max < y_min || curr.y_min > y_max) {
      continue;
    }

    if (!curr.clean && curr.level <= mask_level_) {
      uint64_t mask = cat_masks_[MaskLevelOffset(curr.level) + curr.prefix];
      if ((mask & visible_bits) == 0) {
        continue;
      }
      curr.clean = (mask & hidden_bits) == 0;
    }

    if (curr.clean && curr.x_min >= x_min && curr.x_max <= x_max &&
        curr.y_min >= y_min && curr.y_max <= y_max) {
      int64_t cx1 = fast_cell(curr.x_min, x_min, x_max, inv_cell_x, cols);
      int64_t cx2 = fast_cell(std::max(curr.x_min, curr.x_max - eps_x), x_min,
                              x_max, inv_cell_x, cols);
      int64_t cy1 = fast_cell(curr.y_min, y_min, y_max, inv_cell_y, rows);
      int64_t cy2 = fast_cell(std::max(curr.y_min, curr.y_max - eps_y), y_min,
                              y_max, inv_cell_y, rows);
      bool x_fits = (cx1 == cx2) && (curr.x_max < x_max_data_ ||
                                     fast_cell(curr.x_max, x_min, x_max,
                                               inv_cell_x, cols) == cx1);
      bool y_fits = (cy1 == cy2) && (curr.y_max < y_max_data_ ||
                                     fast_cell(curr.y_max, y_min, y_max,
                                               inv_cell_y, rows) == cy1);
      if (x_fits && y_fits) {
        uint64_t cell_idx =
            static_cast<uint64_t>(cy1) * static_cast<uint64_t>(cols) +
            static_cast<uint64_t>(cx1);
        add_points(cell_idx, curr.hi - curr.lo,
                   curr.lo + (curr.hi - curr.lo) / 2);
        continue;
      }
    }

    if (curr.hi - curr.lo <= 32 || curr.level >= level_t_) {
      for (uint32_t i = curr.lo; i < curr.hi; ++i) {
        if (!curr.clean && IsPointHidden(i, *filter)) {
          continue;
        }
        double px = xs_[i];
        double py = ys_[i];
        if (px >= x_min && px <= x_max && py >= y_min && py <= y_max) {
          int64_t cx = fast_cell(px, x_min, x_max, inv_cell_x, cols);
          int64_t cy = fast_cell(py, y_min, y_max, inv_cell_y, rows);
          add_points(static_cast<uint64_t>(cy) * static_cast<uint64_t>(cols) +
                         static_cast<uint64_t>(cx),
                     1, i);
        }
      }
      continue;
    }

    uint32_t next_level = curr.level + 1;
    double x_mid = 0.5 * (curr.x_min + curr.x_max);
    double y_mid = 0.5 * (curr.y_min + curr.y_max);
    uint32_t shift_bits = 2 * (level_t_ - next_level);

    for (int c = 3; c >= 0; --c) {
      uint64_t child_prefix = (curr.prefix << 2) | static_cast<uint64_t>(c);
      uint32_t lo = level_t_offsets_[child_prefix << shift_bits];
      uint32_t hi = level_t_offsets_[(child_prefix + 1) << shift_bits];
      if (hi > lo) {
        stack[stack_ptr++] = {next_level,
                              child_prefix,
                              lo,
                              hi,
                              (c & 1) ? x_mid : curr.x_min,
                              (c & 1) ? curr.x_max : x_mid,
                              (c & 2) ? y_mid : curr.y_min,
                              (c & 2) ? curr.y_max : y_mid,
                              curr.clean};
      }
    }
  }

  if (use_dense) {
    std::sort(buffers.occupied_cells.begin(), buffers.occupied_cells.end());
    out_results.reserve(buffers.occupied_cells.size());
    for (uint32_t cell_idx : buffers.occupied_cells) {
      const DenseSlot& slot = buffers.dense_slots[cell_idx];
      out_results.push_back({slot.rep_index, slot.count});
      buffers.dense_slots[cell_idx] = {0, 0};
    }
    buffers.occupied_cells.clear();
  } else {
    std::vector<std::pair<uint64_t, DenseSlot>> sorted_cells;
    sorted_cells.reserve(buffers.sparse_slots.size());
    for (auto it = buffers.sparse_slots.GetIterator(); it; ++it) {
      sorted_cells.emplace_back(it.key(), it.value());
    }
    std::sort(sorted_cells.begin(), sorted_cells.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    out_results.reserve(sorted_cells.size());
    for (const auto& entry : sorted_cells) {
      out_results.push_back({entry.second.rep_index, entry.second.count});
    }
    buffers.sparse_slots.Clear();
  }
}

}  // namespace perfetto::trace_processor::scatter_mipmap_operator
