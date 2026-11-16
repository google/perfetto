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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_SCATTER_MIPMAP_OPERATOR_SCATTER_MIPMAP_INDEX_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_SCATTER_MIPMAP_OPERATOR_SCATTER_MIPMAP_INDEX_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/util/bit_vector.h"

#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT)
#include <immintrin.h>
#endif

namespace perfetto::trace_processor::scatter_mipmap_operator {

inline uint64_t Dilate20To40(uint32_t v) {
  uint64_t x = v & 0xFFFFF;
  x = (x | (x << 16)) & 0x0000FFFF0000FFFFULL;
  x = (x | (x << 8)) & 0x00FF00FF00FF00FFULL;
  x = (x | (x << 4)) & 0x0F0F0F0F0F0F0F0FULL;
  x = (x | (x << 2)) & 0x3333333333333333ULL;
  x = (x | (x << 1)) & 0x5555555555555555ULL;
  return x;
}

#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT) && \
    (defined(__x86_64__) || defined(_M_X64))
inline uint64_t EncodeMorton2D(uint32_t qx, uint32_t qy) {
  return _pdep_u64(qx, 0x5555555555555555ULL) |
         (_pdep_u64(qy, 0x5555555555555555ULL) << 1);
}
#else
inline uint64_t EncodeMorton2D(uint32_t qx, uint32_t qy) {
  return Dilate20To40(qx) | (Dilate20To40(qy) << 1);
}
#endif

inline uint64_t DoubleToKey(double d) {
  if (d == 0.0)
    return 0;
  uint64_t u = 0;
  std::memcpy(&u, &d, sizeof(u));
  return u;
}

inline double KeyToDouble(uint64_t u) {
  double d = 0.0;
  std::memcpy(&d, &u, sizeof(d));
  return d;
}

struct CategoryEntry {
  int32_t idx = 0;
  uint64_t count = 0;
  bool is_null = false;
  bool is_other = false;
  std::string str_val;
  int64_t int_val = 0;
  double double_val = 0.0;
};

struct CategoryMapping {
  enum class Type { kString, kInt, kDouble };
  Type type = Type::kString;
  uint32_t auto_top_n = 0;

  base::FlatHashMap<uint32_t, int32_t> str_pool_id_to_idx;
  base::FlatHashMap<uint64_t, int32_t> num_to_idx;
  std::vector<CategoryEntry> categories;

  void PopulateTopN(const base::FlatHashMap<uint32_t, uint32_t>& str_counts,
                    const base::FlatHashMap<uint64_t, uint32_t>& num_counts,
                    uint32_t null_count,
                    uint64_t other_mismatched_count,
                    StringPool* pool);

  int32_t MapString(uint32_t pool_id) const {
    if (const int32_t* it = str_pool_id_to_idx.Find(pool_id)) {
      return *it;
    }
    return static_cast<int32_t>(auto_top_n);
  }

  int32_t MapInt(int64_t val) const {
    if (const int32_t* it = num_to_idx.Find(static_cast<uint64_t>(val))) {
      return *it;
    }
    return static_cast<int32_t>(auto_top_n);
  }

  int32_t MapDouble(double val) const {
    if (const int32_t* it = num_to_idx.Find(DoubleToKey(val))) {
      return *it;
    }
    return static_cast<int32_t>(auto_top_n);
  }
};

struct DataframeColumnReader {
  enum StorageKind { kId, kDouble, kInt64, kUint32, kInt32, kString, kUnknown };
  StorageKind kind = kUnknown;
  const double* double_data = nullptr;
  const int64_t* int64_data = nullptr;
  const uint32_t* uint32_data = nullptr;
  const int32_t* int32_data = nullptr;
  const StringPool::Id* string_data = nullptr;
  const core::BitVector* null_bv = nullptr;
  bool is_sparse_null = false;
  const uint32_t* sparse_prefix_popcount = nullptr;

  bool Init(const dataframe::Column& col) {
    const dataframe::Nullability nullability = col.null_storage.nullability();
    switch (nullability.index()) {
      case dataframe::Nullability::GetTypeIndex<dataframe::NonNull>():
        null_bv = nullptr;
        break;
      case dataframe::Nullability::GetTypeIndex<dataframe::DenseNull>():
        null_bv =
            &col.null_storage.unchecked_get<dataframe::DenseNull>().bit_vector;
        break;
      case dataframe::Nullability::GetTypeIndex<dataframe::SparseNull>():
      case dataframe::Nullability::GetTypeIndex<
          dataframe::SparseNullWithPopcountAlways>():
      case dataframe::Nullability::GetTypeIndex<
          dataframe::SparseNullWithPopcountUntilFinalization>(): {
        const auto& sparse =
            col.null_storage.unchecked_get<dataframe::SparseNull>();
        null_bv = &sparse.bit_vector;
        is_sparse_null = true;
        if (sparse.prefix_popcount_for_cell_get.empty()) {
          return false;
        }
        sparse_prefix_popcount = sparse.prefix_popcount_for_cell_get.data();
        break;
      }
      default:
        return false;
    }

    auto data_ptr = col.storage.data();
    switch (data_ptr.index()) {
      case dataframe::StorageType::GetTypeIndex<dataframe::Id>():
        kind = kId;
        break;
      case dataframe::StorageType::GetTypeIndex<dataframe::Double>():
        kind = kDouble;
        double_data =
            dataframe::Storage::CastDataPtr<dataframe::Double>(data_ptr);
        break;
      case dataframe::StorageType::GetTypeIndex<dataframe::Int64>():
        kind = kInt64;
        int64_data =
            dataframe::Storage::CastDataPtr<dataframe::Int64>(data_ptr);
        break;
      case dataframe::StorageType::GetTypeIndex<dataframe::Uint32>():
        kind = kUint32;
        uint32_data =
            dataframe::Storage::CastDataPtr<dataframe::Uint32>(data_ptr);
        break;
      case dataframe::StorageType::GetTypeIndex<dataframe::Int32>():
        kind = kInt32;
        int32_data =
            dataframe::Storage::CastDataPtr<dataframe::Int32>(data_ptr);
        break;
      case dataframe::StorageType::GetTypeIndex<core::String>():
        kind = kString;
        string_data = dataframe::Storage::CastDataPtr<core::String>(data_ptr);
        break;
      default:
        return false;
    }
    return true;
  }

  PERFETTO_ALWAYS_INLINE bool is_integer() const {
    return kind == kId || kind == kInt64 || kind == kUint32 || kind == kInt32;
  }
  PERFETTO_ALWAYS_INLINE bool is_numeric() const {
    return is_integer() || kind == kDouble;
  }

  PERFETTO_ALWAYS_INLINE bool ResolveIndex(uint32_t row, uint32_t& idx) const {
    idx = row;
    if (PERFETTO_UNLIKELY(null_bv)) {
      if (!null_bv->is_set(row)) {
        return false;
      }
      if (is_sparse_null) {
        idx = static_cast<uint32_t>(sparse_prefix_popcount[row / 64] +
                                    null_bv->count_set_bits_until_in_word(row));
      }
    }
    return true;
  }

  PERFETTO_ALWAYS_INLINE bool GetStringPoolId(uint32_t row,
                                              StringPool::Id& out) const {
    uint32_t idx = 0;
    if (kind != kString || !ResolveIndex(row, idx)) {
      return false;
    }
    out = string_data[idx];
    return !out.is_null();
  }

  PERFETTO_ALWAYS_INLINE bool GetDouble(uint32_t row, double& out) const {
    uint32_t idx = 0;
    if (!ResolveIndex(row, idx)) {
      return false;
    }
    switch (kind) {
      case kDouble:
        out = double_data[idx];
        return std::isfinite(out);
      case kInt64:
        out = static_cast<double>(int64_data[idx]);
        return true;
      case kUint32:
        out = static_cast<double>(uint32_data[idx]);
        return true;
      case kInt32:
        out = static_cast<double>(int32_data[idx]);
        return true;
      case kId:
        out = static_cast<double>(idx);
        return true;
      case kString:
      case kUnknown:
        return false;
    }
    return false;
  }

  PERFETTO_ALWAYS_INLINE bool GetInt64(uint32_t row, int64_t& out) const {
    uint32_t idx = 0;
    if (!ResolveIndex(row, idx)) {
      return false;
    }
    switch (kind) {
      case kInt64:
        out = int64_data[idx];
        return true;
      case kId:
        out = static_cast<int64_t>(idx);
        return true;
      case kUint32:
        out = static_cast<int64_t>(uint32_data[idx]);
        return true;
      case kInt32:
        out = static_cast<int64_t>(int32_data[idx]);
        return true;
      case kDouble: {
        double d = double_data[idx];
        if (!std::isfinite(d) ||
            d < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
            d > static_cast<double>(std::numeric_limits<int64_t>::max())) {
          return false;
        }
        out = static_cast<int64_t>(d);
        return true;
      }
      case kString:
      case kUnknown:
        return false;
    }
    return false;
  }
};

struct SqlRow {
  int64_t id = 0;
  double x = 0.0;
  double y = 0.0;
  bool has_c = false;
  double c = std::numeric_limits<double>::quiet_NaN();
  bool c_is_null = true;
  bool c_is_string = false;
  bool c_is_int = false;
  bool c_is_double = false;
  uint32_t c_str_id = 0;
  int64_t c_int = 0;
  double c_double = 0.0;
};

class ScatterMipmapIndex {
 public:
  struct Result {
    uint32_t rep_index;
    uint32_t count;
  };

  struct DenseSlot {
    uint32_t count = 0;
    uint32_t rep_index = 0;
  };

  // Excludes points by category from a query. Hidden points are neither
  // counted nor chosen as representatives, so cells that only contain hidden
  // points are omitted and mixed cells report only their visible points.
  struct CategoryFilter {
    // hidden[k] is true if category index k (>= 0) is hidden. Only applies to
    // indexes built with categories (top-N mode).
    std::vector<bool> hidden;
    // Hides points whose c is NULL (category -1 / NaN).
    bool hide_null = false;
  };

  struct QueryBuffers {
    std::vector<DenseSlot> dense_slots;
    std::vector<uint32_t> occupied_cells;
    base::FlatHashMap<uint64_t, DenseSlot> sparse_slots;
  };

  ScatterMipmapIndex() = default;

  void IngestAndBuildDataframe(uint32_t n,
                               const DataframeColumnReader* reader_id,
                               const DataframeColumnReader& reader_x,
                               const DataframeColumnReader& reader_y,
                               const DataframeColumnReader* reader_c,
                               CategoryMapping* categories = nullptr,
                               StringPool* pool = nullptr);

  void IngestAndBuildSqlRows(std::vector<SqlRow>& rows,
                             CategoryMapping* categories = nullptr,
                             StringPool* pool = nullptr);

  bool Query(double x_min,
             double x_max,
             double y_min,
             double y_max,
             int64_t cols,
             int64_t rows,
             QueryBuffers& buffers,
             std::vector<Result>& out_results,
             const CategoryFilter* filter = nullptr) const;

  const std::vector<CategoryEntry>& categories() const { return categories_; }
  CategoryMapping::Type categories_type() const { return categories_type_; }

  size_t size() const { return ids_are_32_ ? ids32_.size() : ids64_.size(); }
  bool empty() const { return size() == 0; }

  int64_t id(uint32_t idx) const {
    return ids_are_32_ ? static_cast<int64_t>(ids32_[idx]) : ids64_[idx];
  }
  double x(uint32_t idx) const { return xs_[idx]; }
  double y(uint32_t idx) const { return ys_[idx]; }
  double c(uint32_t idx) const {
    return has_c_ ? static_cast<double>(cs_[idx])
                  : std::numeric_limits<double>::quiet_NaN();
  }

  double x_min_data() const { return x_min_data_; }
  double x_max_data() const { return x_max_data_; }
  double y_min_data() const { return y_min_data_; }
  double y_max_data() const { return y_max_data_; }
  double c_min_data() const { return c_min_data_; }
  double c_max_data() const { return c_max_data_; }
  bool has_c() const { return has_c_; }

 private:
  template <typename PointAccessor>
  void BuildCore(uint32_t n,
                 double x_min,
                 double x_max,
                 double y_min,
                 double y_max,
                 double c_min,
                 double c_max,
                 bool has_c,
                 int64_t min_id,
                 int64_t max_id,
                 PointAccessor& accessor);

  bool TryQueryAligned(double x_min,
                       double x_max,
                       double y_min,
                       double y_max,
                       int64_t cols,
                       int64_t rows,
                       std::vector<Result>& out_results) const;

  void QueryDfs(double x_min,
                double x_max,
                double y_min,
                double y_max,
                int64_t cols,
                int64_t rows,
                QueryBuffers& buffers,
                std::vector<Result>& out_results,
                const CategoryFilter* filter) const;

  // Category presence masks: each point maps to one of 64 bits (see
  // CategoryBit()) and every quadtree node up to `mask_level_` stores the OR
  // of its points' bits. This lets filtered queries skip nodes with no
  // visible points and take the counting fast path for nodes with no hidden
  // points.
  static constexpr uint32_t kMaxMaskLevel = 8;
  static constexpr uint32_t kOverflowBit = 62;  // Category >= 62 / numeric c.
  static constexpr uint32_t kNullBit = 63;
  static constexpr size_t MaskLevelOffset(uint32_t level) {
    return ((size_t{1} << (2 * level)) - 1) / 3;
  }
  uint32_t CategoryBit(uint32_t idx) const;
  bool IsPointHidden(uint32_t idx, const CategoryFilter& filter) const;
  void BuildCategoryMasks();

  int64_t min_id_ = 0;
  int64_t max_id_ = 0;

  bool ids_are_32_ = true;
  std::vector<uint32_t> ids32_;
  std::vector<int64_t> ids64_;
  std::vector<double> xs_;
  std::vector<double> ys_;
  std::vector<float> cs_;

  uint32_t level_t_ = 8;
  std::vector<uint32_t> level_t_offsets_;

  double x_min_data_ = 0.0;
  double x_max_data_ = 0.0;
  double y_min_data_ = 0.0;
  double y_max_data_ = 0.0;
  double c_min_data_ = 0.0;
  double c_max_data_ = 0.0;
  bool has_c_ = false;

  std::vector<CategoryEntry> categories_;
  CategoryMapping::Type categories_type_ = CategoryMapping::Type::kString;
  bool categorical_ = false;

  uint32_t mask_level_ = 0;
  std::vector<uint64_t> cat_masks_;
};

}  // namespace perfetto::trace_processor::scatter_mipmap_operator

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_SCATTER_MIPMAP_OPERATOR_SCATTER_MIPMAP_INDEX_H_
