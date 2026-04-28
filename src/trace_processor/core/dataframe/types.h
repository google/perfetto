#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_TYPES_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/duplicate_types.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/sort_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/paged_vector.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::dataframe {

// Represents an index to speed up operations on the dataframe.
struct Index {
 public:
  Index(std::vector<uint32_t> _columns,
        std::shared_ptr<std::vector<uint32_t>> _permutation_vector)
      : columns_(std::move(_columns)),
        permutation_vector_(std::move(_permutation_vector)) {}

  // Returns a copy of this index.
  Index Copy() const { return *this; }

  // Returns the columns which this index was created on.
  const std::vector<uint32_t>& columns() const { return columns_; }

  // Returns the permutation vector which would to order the `columns` in the
  // dataframe.
  const std::shared_ptr<std::vector<uint32_t>>& permutation_vector() const {
    return permutation_vector_;
  }

 private:
  std::vector<uint32_t> columns_;
  std::shared_ptr<std::vector<uint32_t>> permutation_vector_;
};

// Tag type for Id column data pointers. Id columns don't have backing storage
// (the value is the row index), so we use a distinct pointer type that is
// always null to allow proper type deduction in generic code.
struct IdDataTag {};

// Storage implementation for column data. Provides physical storage
// for different types of column content.
class Storage {
 public:
  // Storage representation for Id columns.
  struct Id {
    uint32_t size;  // Number of rows in the column

    static const IdDataTag* data() { return nullptr; }
  };
  using Uint32 = PagedVector<uint32_t>;
  using Int32 = PagedVector<int32_t>;
  using Int64 = PagedVector<int64_t>;
  using Double = PagedVector<double>;
  using String = PagedVector<StringPool::Id>;

  // Variant of pointers to a column's PagedVector. Used by random-access
  // call sites (cursor materialization, dataframe::CellByRowAndColumn) to
  // read individual cells. Access via `(*p)[idx]` — the PagedVector's
  // operator[] transparently restores evicted pages on demand.
  using DataPointer = std::variant<const IdDataTag*,
                                   PagedVector<uint32_t>*,
                                   PagedVector<int32_t>*,
                                   PagedVector<int64_t>*,
                                   PagedVector<double>*,
                                   PagedVector<StringPool::Id>*>;

  Storage(Storage::Id data) : type_(core::Id{}), data_(data) {}
  Storage(Storage::Uint32 data)
      : type_(core::Uint32{}), data_(std::move(data)) {}
  Storage(Storage::Int32 data) : type_(core::Int32{}), data_(std::move(data)) {}
  Storage(Storage::Int64 data) : type_(core::Int64{}), data_(std::move(data)) {}
  Storage(Storage::Double data)
      : type_(core::Double{}), data_(std::move(data)) {}
  Storage(Storage::String data)
      : type_(core::String{}), data_(std::move(data)) {}

  // Type-safe access to storage with unchecked variant access.
  template <typename T>
  auto& unchecked_get() {
    using U = StorageType::VariantTypeAtIndex<T, Variant>;
    PERFETTO_DCHECK(std::holds_alternative<U>(data_));
    return base::unchecked_get<U>(data_);
  }

  template <typename T>
  const auto& unchecked_get() const {
    using U = StorageType::VariantTypeAtIndex<T, Variant>;
    PERFETTO_DCHECK(std::holds_alternative<U>(data_));
    return base::unchecked_get<U>(data_);
  }

  // Returns a pointer to the underlying PagedVector for the specified type.
  // All access goes through the PagedVector — instructions iterate via
  // WalkPages (scans) or operator[] (random access), both of which
  // transparently restore evicted pages.
  template <typename T>
  auto* unchecked_paged_vector() {
    return &unchecked_get<T>();
  }

  template <typename T>
  const auto* unchecked_paged_vector() const {
    return &unchecked_get<T>();
  }

  // Returns a variant pointing at the column's PagedVector. Returns nullptr
  // (as IdDataTag*) if the storage type is Id (which has no buffer).
  //
  // The pointer is non-const (despite this being a `const` method) because
  // the PagedVector's eviction-state is logically mutable: reading an evicted
  // page transparently restores it. The const_cast captures that the bytes
  // of the column are immutable post-build, but the residency-state
  // bookkeeping isn't.
  DataPointer data() const {
    switch (type_.index()) {
      case StorageType::GetTypeIndex<core::Id>():
        return static_cast<const IdDataTag*>(nullptr);
      case StorageType::GetTypeIndex<core::Uint32>():
        return const_cast<Storage::Uint32*>(
            &base::unchecked_get<Storage::Uint32>(data_));
      case StorageType::GetTypeIndex<core::Int32>():
        return const_cast<Storage::Int32*>(
            &base::unchecked_get<Storage::Int32>(data_));
      case StorageType::GetTypeIndex<core::Int64>():
        return const_cast<Storage::Int64*>(
            &base::unchecked_get<Storage::Int64>(data_));
      case StorageType::GetTypeIndex<core::Double>():
        return const_cast<Storage::Double*>(
            &base::unchecked_get<Storage::Double>(data_));
      case StorageType::GetTypeIndex<core::String>():
        return const_cast<Storage::String*>(
            &base::unchecked_get<Storage::String>(data_));
      default:
        PERFETTO_FATAL("Should not reach here");
    }
  }

  template <typename T>
  static auto* CastDataPtr(const DataPointer& ptr) {
    using U = StorageType::VariantTypeAtIndex<T, DataPointer>;
    return base::unchecked_get<U>(ptr);
  }

  StorageType type() const { return type_; }

 private:
  // Variant containing all possible storage representations.
  using Variant = std::variant<Id, Uint32, Int32, Int64, Double, String>;
  StorageType type_;
  Variant data_;
};

// Stores any information about nulls in the column.
class NullStorage {
 private:
  template <typename T>
  static constexpr uint32_t TypeIndex() {
    return base::variant_index<Variant, T>();
  }

 public:
  // Used for non-null columns which don't need any storage for nulls.
  struct NonNull {};

  // Used for nullable columns where nulls do *not* reserve a slot in `Storage`.
  struct SparseNull {
    // 1 = non-null element in storage.
    // 0 = null with no corresponding entry in storage.
    BitVector bit_vector;

    // For each word in the bit vector, this contains the indices of the
    // corresponding elements in `Storage` that are set.
    //
    // Note: this vector exists for a *very specific* usecase: when we need to
    // handle a GetCell() call on a column which is sparsely null. Note that
    // this *cannot* be used for SetCell columns because that would be O(n) and
    // very inefficient. In those cases, we need to use DenseNull and accept the
    // memory bloat.
    FlexVector<uint32_t> prefix_popcount_for_cell_get;
  };

  // Used for nullable columns where nulls reserve a slot in `Storage`.
  struct DenseNull {
    // 1 = non-null element in storage.
    // 0 = null with entry in storage with unspecified value
    BitVector bit_vector;
  };

  NullStorage(NonNull n) : nullability_(core::NonNull{}), data_(n) {}
  NullStorage(SparseNull s, core::SparseNull = core::SparseNull{})
      : nullability_(core::SparseNull{}), data_(std::move(s)) {}
  NullStorage(SparseNull s, core::SparseNullWithPopcountAlways)
      : nullability_(core::SparseNullWithPopcountAlways{}),
        data_(std::move(s)) {}
  NullStorage(SparseNull s, core::SparseNullWithPopcountUntilFinalization)
      : nullability_(core::SparseNullWithPopcountUntilFinalization{}),
        data_(std::move(s)) {}
  NullStorage(DenseNull d)
      : nullability_(core::DenseNull{}), data_(std::move(d)) {}

  // Type-safe unchecked access to variant data.
  template <typename T>
  auto& unchecked_get() {
    if constexpr (std::is_same_v<T, core::NonNull>) {
      return base::unchecked_get<NonNull>(data_);
    } else if constexpr (std::is_same_v<T, core::SparseNull> ||
                         std::is_same_v<T,
                                        core::SparseNullWithPopcountAlways> ||
                         std::is_same_v<
                             T,
                             core::SparseNullWithPopcountUntilFinalization>) {
      return base::unchecked_get<SparseNull>(data_);
    } else if constexpr (std::is_same_v<T, core::DenseNull>) {
      return base::unchecked_get<DenseNull>(data_);
    } else {
      static_assert(!std::is_same_v<T, T>, "Invalid type");
    }
  }

  template <typename T>
  const auto& unchecked_get() const {
    if constexpr (std::is_same_v<T, core::NonNull>) {
      return base::unchecked_get<NonNull>(data_);
    } else if constexpr (std::is_same_v<T, core::SparseNull> ||
                         std::is_same_v<T,
                                        core::SparseNullWithPopcountAlways> ||
                         std::is_same_v<
                             T,
                             core::SparseNullWithPopcountUntilFinalization>) {
      return base::unchecked_get<SparseNull>(data_);
    } else if constexpr (std::is_same_v<T, core::DenseNull>) {
      return base::unchecked_get<DenseNull>(data_);
    } else {
      static_assert(!std::is_same_v<T, T>, "Invalid type");
    }
  }

  BitVector& GetNullBitVector() {
    switch (data_.index()) {
      case TypeIndex<SparseNull>():
        return unchecked_get<core::SparseNull>().bit_vector;
      case TypeIndex<DenseNull>():
        return unchecked_get<core::DenseNull>().bit_vector;
      default:
        PERFETTO_FATAL("Unsupported overlay type");
    }
  }
  const BitVector& GetNullBitVector() const {
    switch (data_.index()) {
      case TypeIndex<SparseNull>():
        return unchecked_get<core::SparseNull>().bit_vector;
      case TypeIndex<DenseNull>():
        return unchecked_get<core::DenseNull>().bit_vector;
      default:
        PERFETTO_FATAL("Unsupported overlay type");
    }
  }

  const BitVector* MaybeGetNullBitVector() const {
    switch (data_.index()) {
      case TypeIndex<SparseNull>():
        return &unchecked_get<core::SparseNull>().bit_vector;
      case TypeIndex<DenseNull>():
        return &unchecked_get<core::DenseNull>().bit_vector;
      case TypeIndex<NonNull>():
        return nullptr;
      default:
        PERFETTO_FATAL("Unsupported overlay type");
    }
  }

  Nullability nullability() const { return nullability_; }

 private:
  // Variant containing all possible overlay types.
  using Variant = std::variant<NonNull, SparseNull, DenseNull>;
  Nullability nullability_;
  Variant data_;
};

// Holds a specialized alternative representation of the storage of a column.
//
// Should be used to speed up very common operations on columns that have
// specific properties.
struct SpecializedStorage {
 public:
  // Special data structure capable of giving very fast results for equality
  // constraints on sorted, non-duplicate columns with not-too large values.
  // This is very common when joining two tables together by id.
  //
  // Usable in situations where the column has all the following properties:
  //  1) It's non-null.
  //  2) It's sorted.
  //  3) It has no duplicate values.
  //  4) The max(value) is "reasonably small".
  //     - as the the memory used will be O(max(value)) *not* O(size(column)).
  struct SmallValueEq {
    // BitVector with 1s representing the presence of a value in the
    // column. The value is the index of the value in the column.
    //
    // For example, if the column has values [1, 2, 3], then the bit vector
    // will have 1s at indices 1, 2, and 3.
    BitVector bit_vector;

    // Cumulative count of set bits in the bit vector. Key to allowing O(1)
    // equality queries.
    //
    // See BitVector::PrefixPopcount() for details.
    Slab<uint32_t> prefix_popcount;
  };

  SpecializedStorage() = default;
  SpecializedStorage(SmallValueEq data) : data_(std::move(data)) {}

  template <typename T>
  bool Is() const {
    return std::holds_alternative<T>(data_);
  }

  template <typename T>
  const T& unchecked_get() const {
    PERFETTO_DCHECK(Is<T>());
    return base::unchecked_get<T>(data_);
  }

 private:
  using Variant = std::variant<std::monostate, SmallValueEq>;
  Variant data_;
};

// Represents a complete column in the dataframe.
struct Column {
  Storage storage;
  NullStorage null_storage;
  SortState sort_state;
  DuplicateState duplicate_state;
  SpecializedStorage specialized_storage = SpecializedStorage{};
  uint32_t mutations = 0;

  // Estimated number of distinct values in the column, computed at
  // finalization. Drives equality-filter selectivity in the cost model
  // (result rows ~= row_count / estimated_distinct). 0 means unknown, in
  // which case the planner falls back to a data-blind heuristic.
  uint32_t estimated_distinct = 0;
};

}  // namespace perfetto::trace_processor::core::dataframe

namespace perfetto::trace_processor {

// Namespace alias for ergonomics.
namespace dataframe = core::dataframe;

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_TYPES_H_
