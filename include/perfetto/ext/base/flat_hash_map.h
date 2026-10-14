/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_FLAT_HASH_MAP_H_
#define INCLUDE_PERFETTO_EXT_BASE_FLAT_HASH_MAP_H_

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/bits.h"
#include "perfetto/ext/base/flat_hash_map_v1.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/public/compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT)
#include <immintrin.h>
#endif

namespace perfetto::base {

// A Swiss Table-style open-addressing hashmap implementation.
// Inspired by absl::flat_hash_map, this uses a metadata array of control bytes
// to enable fast SIMD-accelerated probing.
//
// Key design choices:
// - Control bytes: Each slot has a 1-byte tag (7-bit H2 hash or special marker)
//   stored in a separate array, enabling fast group-based matching.
// - SIMD acceleration: On x64, uses SSE instructions to match 16 control bytes
//   in parallel. Falls back to SWAR (SIMD Within A Register) on other platforms
//   matching 8 bytes at a time.
// - Triangular probing: Uses the sequence 0, 16, 48, 96, ... (like Absl) to
//   probe groups of slots, ensuring good cache behavior.
// - Pointers are NOT stable: Neither keys nor values have stable addresses
//   across insertions that trigger rehashing.
//
// See also: FlatHashMapV1 in flat_hash_map_v1.h for the older implementation
// using traditional open-addressing with configurable probing strategies.

// Non-templated base class to hold helpers for FlatHashMapV2.
namespace flat_hash_map_v2_internal {

// Helper to detect if a hasher has is_transparent defined.
template <typename, typename = void>
struct HasIsTransparent : std::false_type {};

template <typename H>
struct HasIsTransparent<H, std::void_t<typename H::is_transparent>>
    : std::true_type {};

// Equality comparator trait.
template <typename T>
struct HashEq : public std::equal_to<T> {};

// Specialization for std::string to compare via std::string_view.
//
// This exists because after benchmarking, it turns out libc++ has an
// "optimization" for std::string equality that does something byte by
// byte comparision for short strings but this is slower than just memcmp.
//
// This helps close the gap to absl::flat_hash_map for string keys.
template <>
struct HashEq<std::string> {
  bool operator()(const std::string& a, const std::string& b) const {
    return std::string_view(a) == std::string_view(b);
  }
  bool operator()(const std::string& a, const std::string_view& b) const {
    return std::string_view(a) == b;
  }
  bool operator()(const std::string& a, base::StringView b) const {
    return base::StringView(a) == b;
  }
  bool operator()(const std::string& a, const char* b) const {
    return std::string_view(a) == std::string_view(b);
  }
};

// Specialization for double to prevent spurious -Wfloat-equal warnings.
//
// Trace processor legitimately has a need to compare with exact equality
template <>
struct HashEq<double> {
  bool operator()(double a, double b) const { return std::equal_to<>()(a, b); }
};

// Helper to check if a lookup key type K is allowed.
// Returns true if:
// 1. Hasher is transparent, can hash K, and Eq can compare Key and K, OR
// 2. Hasher is not transparent and K can be implicitly converted to Key.
template <typename K, typename Key, typename Hasher, typename Eq>
constexpr bool IsLookupKeyAllowed() {
  if constexpr (HasIsTransparent<Hasher>::value) {
    return std::is_invocable_v<Hasher, const K&> &&
           std::is_invocable_v<Eq, const Key&, const K&>;
  } else if constexpr (std::is_convertible_v<K, Key>) {
    return true;
  } else {
    return false;
  }
}

// Swiss Table control byte encoding:
// - Empty:   0x80 (10000000) - MSB set, easy to detect with sign bit
// - Deleted: 0xFE (11111110) - MSB set
// - Full:    0x00-0x7F - MSB clear, stores 7-bit H2 hash
static constexpr uint8_t kFreeSlot = 0x80;   // Empty slot
static constexpr uint8_t kTombstone = 0xFE;  // Deleted slot

// The default load limit percent before growing the table.
static constexpr int kDefaultLoadLimitPct = 75;

// Abstraction over a group of control bytes that enables batch operations.
// On x64, uses SSE to match 16 control bytes in parallel.
// On other platforms, uses SWAR (SIMD Within A Register) for 8 bytes.
//
// Match() returns an iterator over slots whose control byte matches h2.
// MatchEmpty() returns an iterator over empty slots (kFreeSlot).
// MatchEmptyOrDeleted() returns an iterator over empty or deleted slots.
#if PERFETTO_BUILDFLAG(PERFETTO_X64_CPU_OPT)
struct Group {
 public:
  // Group size 16 for x64 SSE
  static constexpr size_t kSize = 16;

  // Iterates over set bits in a match mask. Each set bit indicates a slot
  // in the group that matched the search criteria. Call Next() to get the
  // index of each matching slot.
  struct Iterator {
   public:
    PERFETTO_ALWAYS_INLINE explicit Iterator(uint16_t mask) : mask_(mask) {}
    PERFETTO_ALWAYS_INLINE explicit operator bool() const { return mask_; }

    PERFETTO_ALWAYS_INLINE size_t Next() {
      auto idx = static_cast<size_t>(CountTrailZeros(mask_));
      mask_ &= static_cast<uint16_t>(mask_ - 1);
      return idx;
    }

   private:
    uint16_t mask_;
  };

  PERFETTO_ALWAYS_INLINE explicit Group(const uint8_t* pos) {
    ctrl_ = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pos));
  }

  PERFETTO_ALWAYS_INLINE Iterator Match(uint8_t h2) const {
    auto match = _mm_cmpeq_epi8(ctrl_, _mm_set1_epi8(static_cast<char>(h2)));
    return Iterator(static_cast<uint16_t>(_mm_movemask_epi8(match)));
  }

  PERFETTO_ALWAYS_INLINE Iterator MatchEmpty() const {
    return Iterator(
        static_cast<uint16_t>(_mm_movemask_epi8(_mm_sign_epi8(ctrl_, ctrl_))));
  }

  PERFETTO_ALWAYS_INLINE Iterator MatchEmptyOrDeleted() const {
    return Iterator(static_cast<uint16_t>(_mm_movemask_epi8(ctrl_)));
  }

 private:
  __m128i ctrl_;
};
#else
// SWAR fallback: processes 8 control bytes at a time using 64-bit arithmetic.
struct Group {
 public:
  // Group size 8 for ARM and other platforms
  static constexpr size_t kSize = 8;

  // Iterates over set bits in a sparse 64-bit mask. Each set MSB indicates
  // a matching byte in the group. Call Next() to get the index of each
  // matching slot.
  struct Iterator {
   public:
    PERFETTO_ALWAYS_INLINE explicit Iterator(uint64_t mask) : mask_(mask) {}
    PERFETTO_ALWAYS_INLINE explicit operator bool() const { return mask_; }
    PERFETTO_ALWAYS_INLINE size_t Next() {
      // Count zeros and divide by 8 (shift 3)
      // 0x80 (Byte 0) -> CTZ 7  -> 7>>3 = 0
      // 0x8000 (Byte 1) -> CTZ 15 -> 15>>3 = 1
      size_t idx = static_cast<size_t>(CountTrailZeros(mask_) >> 3);
      mask_ &= mask_ - 1;  // Clear lowest set bit
      return idx;
    }

   private:
    uint64_t mask_;
  };

  PERFETTO_ALWAYS_INLINE explicit Group(const uint8_t* pos) {
    memcpy(&ctrl_, pos, sizeof(ctrl_));
  }

  PERFETTO_ALWAYS_INLINE Iterator Match(uint8_t h2) const {
    uint64_t x = ctrl_ ^ (kLsbs * h2);
    return Iterator((x - kLsbs) & ~x & kMsbs);
  }

  PERFETTO_ALWAYS_INLINE Iterator MatchEmpty() const {
    // 0x80 check (Empty)
    return Iterator((ctrl_ & ~(ctrl_ << 6)) & kMsbs);
  }

  PERFETTO_ALWAYS_INLINE Iterator MatchEmptyOrDeleted() const {
    // 0x80 or 0xFE check (Empty or Deleted)
    return Iterator((ctrl_ & ~(ctrl_ << 7)) & kMsbs);
  }

 private:
  static constexpr uint64_t kLsbs = 0x0101010101010101ULL;
  static constexpr uint64_t kMsbs = 0x8080808080808080ULL;

  uint64_t ctrl_;
};
#endif

// The state and the operations of FlatHashMapV2 which don't depend on the key
// and value types. Not templated, and the heavier operations are defined in
// flat_hash_map.cc, so that all maps share one copy of the code.
class FlatHashMapV2Base {
 protected:
  // Tracks growth capacity and whether any deletions have occurred.
  // Using bitfields like absl's GrowthInfo to avoid manual bit manipulation.
  struct GrowthInfo {
    uint64_t growth_left : 63;
    uint64_t has_tombstones : 1;
  };

  // The table being moved out of while growing.
  struct OldTable {
    std::unique_ptr<uint8_t[]> storage;
    const uint8_t* ctrl;
    uint8_t* slots;
    size_t capacity;
    size_t size;
  };

  // The number of cloned control bytes after the main control byte array.
  static constexpr size_t kNumClones = Group::kSize - 1;

  explicit FlatHashMapV2Base(int load_limit_pct)
      : load_limit_percent_(load_limit_pct) {}

  FlatHashMapV2Base(FlatHashMapV2Base&& other) noexcept
      : storage_(std::move(other.storage_)),
        capacity_(other.capacity_),
        size_(other.size_),
        growth_info_(other.growth_info_),
        load_limit_percent_(other.load_limit_percent_),
        ctrl_(other.ctrl_),
        slots_(other.slots_) {
    other.capacity_ = 0;
    other.size_ = 0;
    other.growth_info_ = {0, 0};
    other.load_limit_percent_ = kDefaultLoadLimitPct;
    other.ctrl_ = nullptr;
    other.slots_ = nullptr;
  }

  // Swiss Table hash splitting (matching absl):
  // H1 = upper bits for bucket index
  // H2 = lower 7 bits for tag
  // This ensures H1 and H2 are independent, avoiding tag collisions within
  // buckets. The seed XOR prevents clustering when hash values have patterns
  // (e.g., sequential keys)
  static constexpr size_t H1(size_t hash) { return (hash >> 7); }
  static constexpr uint8_t H2(size_t hash) { return hash & 0x7F; }

  // Doesn't call destructors.
  void Reset(size_t n, bool reallocate, size_t slot_size, size_t slot_align);

  // Find first empty OR tombstone slot for insertion.
  // Called when has_tombstones is set to find an earlier tombstone that can
  // be reused instead of taking a new empty slot.
  size_t FindFirstEmptyOrTombstone(size_t key_hash) const;

  PERFETTO_ALWAYS_INLINE size_t
  FindFirstEmptyOrTombstoneImpl(size_t key_hash) const {
    const size_t cap_mask = capacity_ - 1;
    size_t offset = H1(key_hash) & cap_mask;
    size_t probe_size = 0;
    while (true) {
      Group group(ctrl_ + offset);
      if (auto it = group.MatchEmptyOrDeleted(); PERFETTO_LIKELY(it)) {
        return (offset + it.Next()) & cap_mask;
      }
      probe_size += Group::kSize;
      offset = (offset + probe_size) & cap_mask;
    }
  }

  // Swaps in an empty table twice as large and returns the old one, from
  // which the caller must move the entries before calling FinishGrow().
  OldTable Grow(size_t slot_size, size_t slot_align);

  void FinishGrow(size_t new_size) {
    PERFETTO_CHECK(growth_info_.growth_left >= new_size);
    size_ = new_size;
    growth_info_.growth_left -= new_size;
  }

  // Set control byte and update clone if needed
  PERFETTO_ALWAYS_INLINE void SetCtrl(size_t i, uint8_t h) {
    ctrl_[i] = h;
    // Update clone if this is one of the first kNumClones entries
    if (PERFETTO_UNLIKELY(i < kNumClones)) {
      ctrl_[capacity_ + i] = h;
    }
  }

  // Owns the actual memory with the following layout:
  //
  // [Control bytes]
  //   |capacity_| bytes for control bytes.
  //   kNumClones (15 or 7) bytes for control byte clones (*).
  //   No alignment required (accessed at arbitrary byte offsets).
  //
  // [Padding for Slot alignment]
  //
  // [Slots]
  //   capacity_ * sizeof(Slot): contains key-value pairs.
  //   Must be aligned to alignof(Slot).
  //
  // (*) Control byte clones: The first kNumClones control bytes are duplicated
  // at the end of the control array. This allows SIMD operations to read a full
  // group (16 or 8 bytes) starting from any position without bounds checking,
  // even near the end of the array.
  std::unique_ptr<uint8_t[]> storage_;

  size_t capacity_ = 0;
  size_t size_ = 0;

  // Slots remaining + has_deleted flag
  GrowthInfo growth_info_{0, 0};

  // Load factor limit in % of |capacity_|.
  int load_limit_percent_ = kDefaultLoadLimitPct;

  // Cached pointers for fast access (like absl::flat_hash_map)
  // These are updated whenever storage is allocated/reallocated.
  uint8_t* ctrl_ = nullptr;   // Points to control bytes
  uint8_t* slots_ = nullptr;  // Points to slot array
};

}  // namespace flat_hash_map_v2_internal

// Hash and equality functors which treat ASCII strings case-insensitively.
// Pass as the Hasher/Eq parameters of FlatHashMapV2 for maps keyed by
// case-insensitive identifiers (e.g. SQL object names). Transparent, so
// lookups accept std::string_view or const char* without constructing a
// std::string key.
struct CaseInsensitiveHash {
  using is_transparent = void;
  uint64_t operator()(std::string_view value) const {
    MurmurHashCombiner combiner;
    for (char c : value) {
      combiner.Combine(Lowercase(c));
    }
    return combiner.digest();
  }
};

struct CaseInsensitiveEq {
  using is_transparent = void;
  bool operator()(std::string_view lhs, std::string_view rhs) const {
    return CaseInsensitiveEqual(lhs, rhs);
  }
};

template <typename Key,
          typename Value,
          typename Hasher = base::MurmurHash<Key>,
          typename Eq = flat_hash_map_v2_internal::HashEq<Key>>
class FlatHashMapV2 : private flat_hash_map_v2_internal::FlatHashMapV2Base {
 private:
  // Import constants from internal namespace.
  static constexpr uint8_t kFreeSlot = flat_hash_map_v2_internal::kFreeSlot;
  static constexpr uint8_t kTombstone = flat_hash_map_v2_internal::kTombstone;
  static constexpr int kDefaultLoadLimitPct =
      flat_hash_map_v2_internal::kDefaultLoadLimitPct;

  // Slot structure holds both key and value
  struct Slot {
    Key key;
    Value value;
  };

  // Whether |K| is a type other than Key which, with a transparent Hasher,
  // is hashed and compared to Key as is (e.g. a std::string_view looked up in
  // a map keyed by std::string), sparing the construction of a Key.
  template <typename K>
  static constexpr bool IsHeterogeneousKey() {
    using RawK = std::remove_cv_t<std::remove_reference_t<K>>;
    if constexpr (std::is_same_v<RawK, Key>) {
      return false;
    } else {
      return flat_hash_map_v2_internal::HasIsTransparent<Hasher>::value &&
             flat_hash_map_v2_internal::IsLookupKeyAllowed<RawK, Key, Hasher,
                                                           Eq>();
    }
  }

 public:
  class Iterator {
   public:
    explicit Iterator(const uint8_t* ctrl, const uint8_t* ctrl_end, Slot* slot)
        : ctrl_(ctrl), ctrl_end_(ctrl_end), slot_(slot) {
      FindNextNonFree();
    }
    ~Iterator() = default;
    Iterator(const Iterator&) = default;
    Iterator& operator=(const Iterator&) = default;
    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) noexcept = default;

    const Key& key() { return slot_->key; }
    Value& value() { return slot_->value; }
    const Key& key() const { return slot_->key; }
    const Value& value() const { return slot_->value; }

    explicit operator bool() const { return ctrl_ != ctrl_end_; }
    Iterator& operator++() {
      PERFETTO_DCHECK(ctrl_ != ctrl_end_);
      ++ctrl_;
      ++slot_;
      FindNextNonFree();
      return *this;
    }

   private:
    void FindNextNonFree() {
      for (; ctrl_ != ctrl_end_; ++ctrl_, ++slot_) {
        const uint8_t cur_ctrl = *ctrl_;
        if (cur_ctrl != kFreeSlot && cur_ctrl != kTombstone)
          return;
      }
    }
    const uint8_t* ctrl_ = nullptr;
    const uint8_t* ctrl_end_ = nullptr;
    Slot* slot_ = nullptr;
  };  // Iterator

  explicit FlatHashMapV2(size_t initial_capacity = 0,
                         int load_limit_pct = kDefaultLoadLimitPct)
      : FlatHashMapV2Base(load_limit_pct) {
    if (initial_capacity > 0) {
      Reset(initial_capacity, true);
    }
  }

  // We are calling Clear() so that the destructors for the inserted entries are
  // called (unless they are trivial, in which case it will be a no-op).
  ~FlatHashMapV2() { Clear(); }

  FlatHashMapV2(FlatHashMapV2&& other) noexcept
      : FlatHashMapV2Base(std::move(other)) {}

  FlatHashMapV2& operator=(FlatHashMapV2&& other) noexcept {
    this->~FlatHashMapV2();
    new (this) FlatHashMapV2(std::move(other));
    return *this;
  }

  FlatHashMapV2(const FlatHashMapV2&) = delete;
  FlatHashMapV2& operator=(const FlatHashMapV2&) = delete;

  // Find(), Erase(), Insert() and operator[] take a Key, which the caller
  // converts to as needed. With a transparent Hasher, they also take any type
  // it can hash and compare to Key as is (see IsHeterogeneousKey()).

  PERFETTO_ALWAYS_INLINE Value* Find(const Key& key) const {
    return FindImpl(key);
  }
  template <typename K, typename = std::enable_if_t<IsHeterogeneousKey<K>()>>
  PERFETTO_ALWAYS_INLINE Value* Find(const K& key) const {
    return FindImpl(key);
  }

  bool Erase(const Key& key) { return EraseImpl(key); }
  template <typename K, typename = std::enable_if_t<IsHeterogeneousKey<K>()>>
  bool Erase(const K& key) {
    return EraseImpl(key);
  }

  PERFETTO_ALWAYS_INLINE std::pair<Value*, bool> Insert(const Key& key,
                                                        Value value) {
    return InsertImpl(key, std::move(value));
  }
  PERFETTO_ALWAYS_INLINE std::pair<Value*, bool> Insert(Key&& key,
                                                        Value value) {
    return InsertImpl(std::move(key), std::move(value));
  }
  // Constructs the Key only if |key| is absent.
  template <typename K, typename = std::enable_if_t<IsHeterogeneousKey<K>()>>
  PERFETTO_ALWAYS_INLINE std::pair<Value*, bool> Insert(K&& key, Value value) {
    return InsertImpl(std::forward<K>(key), std::move(value));
  }

  Value& operator[](const Key& key) { return *Insert(key, Value{}).first; }
  Value& operator[](Key&& key) {
    return *Insert(std::move(key), Value{}).first;
  }
  template <typename K, typename = std::enable_if_t<IsHeterogeneousKey<K>()>>
  Value& operator[](K&& key) {
    return *Insert(std::forward<K>(key), Value{}).first;
  }

  void Clear() {
    // Avoid trivial heap operations on zero-capacity std::move()-d objects.
    if (PERFETTO_UNLIKELY(capacity_ == 0)) {
      return;
    }
    for (size_t i = 0; i < capacity_; ++i) {
      const uint8_t tag = ctrl_[i];
      if (tag == kFreeSlot || tag == kTombstone) {
        continue;
      }
      slots()[i].key.~Key();
      slots()[i].value.~Value();
    }
    Reset(capacity_, false);
  }

  Iterator GetIterator() { return Iterator(ctrl_, ctrl_ + capacity_, slots()); }
  Iterator GetIterator() const {
    return Iterator(ctrl_, ctrl_ + capacity_, slots());
  }

  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }

 private:
  // Result struct for FindOrPrepareInsert - avoids bit manipulation overhead
  struct FindResult {
    uint64_t idx : 63;
    uint64_t needs_insert : 1;
  };

  // Not found sentinel (must fit in 63-bit FindResult.idx)
  static constexpr size_t kNotFound = std::numeric_limits<size_t>::max() >> 1;

  using Group = flat_hash_map_v2_internal::Group;

  // Searches for a key in the table. This function IGNORES tombstones during
  // the search - it only stops at empty slots (kFreeSlot) or matching keys.
  //
  // Why ignore tombstones? In Swiss Tables, tombstones mark deleted entries but
  // must be skipped during lookup because the key we're searching for may have
  // been inserted AFTER the tombstone was created (i.e., the key's probe
  // sequence may have skipped over that tombstone). Only an empty slot
  // definitively proves the key doesn't exist.
  //
  // Returns FindResult with idx and whether the key needs to be inserted:
  // - If key is found: {idx, false} where idx is the slot containing the key.
  // - If key not found and ForInsert=true: {empty_idx, true} where empty_idx
  //   is the index of the first EMPTY slot encountered.
  // - If key not found and ForInsert=false: {kNotFound, true}.
  //
  // IMPORTANT for insertion (ForInsert=true): The returned empty_idx is NOT
  // necessarily the best slot to insert into! There may be an earlier tombstone
  // in the probe sequence that should be reused to avoid wasting slots. When
  // has_tombstones is set, the caller must make a SECOND pass by calling
  // FindFirstEmptyOrTombstone() to find the actual insertion slot.
  template <bool ForInsert, typename K = Key>
  PERFETTO_ALWAYS_INLINE FindResult
  FindSlotIgnoringTombstones(const K& key, size_t key_hash, uint8_t h2) const {
    static_assert(
        flat_hash_map_v2_internal::IsLookupKeyAllowed<K, Key, Hasher, Eq>(),
        "Heterogeneous lookup requires Hasher to define is_transparent and "
        "support hashing the lookup key type, and Eq to compare Key and K. "
        "Without a transparent Hasher, K must be convertible to Key.");

    if (PERFETTO_UNLIKELY(ctrl_ == nullptr)) {
      return {kNotFound, true};
    }

    const size_t cap_mask = capacity_ - 1;
    size_t offset = H1(key_hash) & cap_mask;
    size_t probe_size = 0;
    const uint8_t* ctrl = ctrl_;

    // Prefetch control bytes (like Absl's prefetch_heap_block).
    // Use locality hint 3 (high temporal locality) for better L1 cache usage.
    __builtin_prefetch(ctrl_ + offset, 0, 3);

    while (true) {
      // Prefetch slots at current probe offset (like Absl).
      __builtin_prefetch(slots() + offset, 0, 3);

      Group group(ctrl + offset);

      // Match H2 tags in this group.
      for (auto it = group.Match(h2); PERFETTO_LIKELY(it);) {
        // Must mask because offset + it.Next() can exceed capacity when
        // group straddles the table boundary (using cloned control bytes).
        size_t idx = (offset + it.Next()) & cap_mask;
        if (PERFETTO_LIKELY(Eq{}(slots()[idx].key, key))) {
          return {idx, false};  // Found
        }
      }

      // Check for empty slot (NOT tombstones). If we find an empty slot, the
      // key cannot exist in the table (empty slots terminate probe chains).
      if (auto it = group.MatchEmpty(); PERFETTO_LIKELY(it)) {
        if constexpr (ForInsert) {
          size_t empty_idx = (offset + it.Next()) & cap_mask;
          return {empty_idx, true};  // Not found - empty slot for insertion
        } else {
          return {kNotFound, true};  // Not found - no need to compute slot
        }
      }

      // Triangular probing (like Absl): 0, 16, 48, 96, ...
      probe_size += Group::kSize;
      offset = (offset + probe_size) & cap_mask;

      // Should never happen with load limit.
      PERFETTO_DCHECK(probe_size <= capacity_);
    }
  }

  // Only constructing the key and value is inlined: finding the slot is out of
  // line in FindOrPrepareInsert(), compiled once per lookup key type rather
  // than per call site or per value category of |key|.
  template <typename K>
  PERFETTO_ALWAYS_INLINE std::pair<Value*, bool> InsertImpl(K&& key,
                                                            Value&& value) {
    const std::remove_reference_t<K>& lookup_key = key;
    FindResult res = FindOrPrepareInsert(lookup_key);
    Slot* slot = &slots()[res.idx];
    const bool inserted = res.needs_insert;
    if (inserted) {
      new (&slot->key) Key(std::forward<K>(key));
      new (&slot->value) Value(std::move(value));
    }
    return {&slot->value, inserted};
  }

  // Out of line, compiled once per lookup key type.
  template <typename K>
  PERFETTO_NO_INLINE Value* FindImpl(const K& key) const {
    size_t key_hash = Hasher{}(key);
    uint8_t h2 = H2(key_hash);
    FindResult res = FindSlotIgnoringTombstones<false>(key, key_hash, h2);
    if (PERFETTO_UNLIKELY(res.needs_insert)) {
      return nullptr;
    }
    return &slots()[res.idx].value;
  }

  template <typename K>
  PERFETTO_NO_INLINE bool EraseImpl(const K& key) {
    size_t key_hash = Hasher{}(key);
    uint8_t h2 = H2(key_hash);
    FindResult res = FindSlotIgnoringTombstones<false>(key, key_hash, h2);
    if (PERFETTO_UNLIKELY(res.needs_insert)) {
      return false;
    }
    PERFETTO_DCHECK(size_ > 0);
    SetCtrl(res.idx, kTombstone);
    slots()[res.idx].key.~Key();
    slots()[res.idx].value.~Value();
    size_--;
    growth_info_.has_tombstones = 1;
    return true;
  }

  // Returns the slot holding |key| if present. Otherwise claims a slot for it,
  // growing the table if needed, and the caller must construct into it.
  template <typename K>
  PERFETTO_NO_INLINE FindResult FindOrPrepareInsert(const K& key) {
    size_t key_hash = Hasher{}(key);
    uint8_t h2 = H2(key_hash);
    FindResult res = FindSlotIgnoringTombstones<true>(key, key_hash, h2);
    if (PERFETTO_UNLIKELY(!res.needs_insert)) {
      return res;
    }
    if (PERFETTO_UNLIKELY(growth_info_.growth_left == 0)) {
      GrowAndRehash();
      // After rehash, table has no tombstones. Find an empty slot directly
      // instead of recursing (which would prevent inlining).
      res.idx = FindFirstEmptyOrTombstone(key_hash);
    }
    PERFETTO_DCHECK(res.idx != kNotFound);
    size_t insert_idx = res.idx;
    bool is_freeslot = true;
    if (PERFETTO_UNLIKELY(growth_info_.has_tombstones)) {
      insert_idx = FindFirstEmptyOrTombstone(key_hash);
      is_freeslot = ctrl_[insert_idx] != kTombstone;
    }
    SetCtrl(insert_idx, h2);
    size_++;
    if (is_freeslot) {
      growth_info_.growth_left--;
    }
    return {insert_idx, true};
  }

  // Allocating the new table is shared by all maps in Grow(): only moving the
  // entries over, which needs their types, is compiled per map.
  PERFETTO_NO_INLINE void GrowAndRehash() {
    OldTable old = Grow(sizeof(Slot), alignof(Slot));
    Slot* old_slots = reinterpret_cast<Slot*>(old.slots);
    // Keys are unique and the new table has no tombstones, so each entry goes
    // straight into the first empty slot of its probe sequence.
    size_t new_size = 0;
    for (size_t i = 0; i < old.capacity; ++i) {
      if (uint8_t t = old.ctrl[i]; t == kFreeSlot || t == kTombstone) {
        continue;
      }
      Slot& old_slot = old_slots[i];
      size_t key_hash = Hasher{}(old_slot.key);
      size_t idx = FindFirstEmptyOrTombstoneImpl(key_hash);
      new (&slots()[idx].key) Key(std::move(old_slot.key));
      new (&slots()[idx].value) Value(std::move(old_slot.value));
      SetCtrl(idx, H2(key_hash));
      old_slot.key.~Key();  // Destroy the old objects.
      old_slot.value.~Value();
      new_size++;
    }
    PERFETTO_DCHECK(new_size == old.size);
    FinishGrow(new_size);
  }

  // Doesn't call destructors. Use Clear() for that.
  void Reset(size_t n, bool reallocate) {
    FlatHashMapV2Base::Reset(n, reallocate, sizeof(Slot), alignof(Slot));
  }

  PERFETTO_ALWAYS_INLINE Slot* slots() const {
    return reinterpret_cast<Slot*>(slots_);
  }
};

// Alias FlatHashMap to FlatHashMapV1 for backward compatibility.
//
// TODO(lalitm): Once FlatHashMapV2 is fully tested and verified, switch this
// to FlatHashMapV2.
template <typename Key,
          typename Value,
          typename Hasher = MurmurHash<Key>,
          typename Probe = QuadraticProbe,
          bool AppendOnly = false>
using FlatHashMap = FlatHashMapV1<Key, Value, Hasher, Probe, AppendOnly>;

}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_FLAT_HASH_MAP_H_
