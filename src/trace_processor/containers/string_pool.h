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

#ifndef SRC_TRACE_PROCESSOR_CONTAINERS_STRING_POOL_H_
#define SRC_TRACE_PROCESSOR_CONTAINERS_STRING_POOL_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/trace_processor/containers/null_term_string_view.h"

namespace perfetto::trace_processor {

// Interns strings in a string pool and hands out compact StringIds which can
// be used to retrieve the string in O(1).
class StringPool {
 public:
  struct Id {
    Id() = default;

    constexpr bool operator==(const Id& other) const { return other.id == id; }
    constexpr bool operator!=(const Id& other) const {
      return !(other == *this);
    }
    constexpr bool operator<(const Id& other) const { return id < other.id; }

    constexpr bool is_null() const { return id == 0u; }

    constexpr bool is_large_string() const {
      return id & kLargeStringFlagBitMask;
    }

    constexpr uint32_t block_offset() const { return id & kBlockOffsetBitMask; }

    constexpr uint32_t block_index() const {
      return (id & kBlockIndexBitMask) >> kNumBlockOffsetBits;
    }

    constexpr uint32_t large_string_index() const {
      PERFETTO_DCHECK(is_large_string());
      return id & ~kLargeStringFlagBitMask;
    }

    constexpr uint32_t raw_id() const { return id; }

    static constexpr Id LargeString(size_t index) {
      PERFETTO_DCHECK(index <= static_cast<uint32_t>(index));
      PERFETTO_DCHECK(!(index & kLargeStringFlagBitMask));
      return Id(kLargeStringFlagBitMask | static_cast<uint32_t>(index));
    }

    static constexpr Id BlockString(size_t index, uint32_t offset) {
      PERFETTO_DCHECK(index < (1u << (kNumBlockIndexBits + 1)));
      PERFETTO_DCHECK(offset < (1u << (kNumBlockOffsetBits + 1)));
      return Id(~kLargeStringFlagBitMask &
                (static_cast<uint32_t>(index << kNumBlockOffsetBits) |
                 (offset & kBlockOffsetBitMask)));
    }

    static constexpr Id Raw(uint32_t raw) { return Id(raw); }

    static constexpr Id Null() { return Id(0u); }

   private:
    constexpr explicit Id(uint32_t i) : id(i) {}

    uint32_t id;
  };

  // Iterator over the strings in the pool.
  class Iterator {
   public:
    explicit Iterator(const StringPool*);

    explicit operator bool() const;
    Iterator& operator++();

    NullTermStringView StringView();
    Id StringId();

   private:
    const StringPool* pool_ = nullptr;
    uint32_t block_index_ = 0;
    uint32_t block_offset_ = 0;
    uint32_t large_strings_index_ = 0;
  };

  StringPool();
  ~StringPool();

  // Allow std::move().
  StringPool(StringPool&&) noexcept;
  StringPool& operator=(StringPool&&) noexcept;

  // Disable implicit copy.
  StringPool(const StringPool&) = delete;
  StringPool& operator=(const StringPool&) = delete;

  Id InternString(base::StringView str) {
    if (str.data() == nullptr)
      return Id::Null();

    auto hash = str.Hash();

    // Perform a hashtable insertion with a null ID just to check if the string
    // is already inserted. If it's not, overwrite 0 with the actual Id.
    auto it_and_inserted = string_index_.Insert(hash, Id());
    Id* id = it_and_inserted.first;
    if (!it_and_inserted.second) {
      PERFETTO_DCHECK(Get(*id) == str);
      return *id;
    }
    *id = InsertString(str, hash);
    return *id;
  }

  std::optional<Id> GetId(base::StringView str) const {
    if (str.data() == nullptr)
      return Id::Null();

    auto hash = str.Hash();
    Id* id = string_index_.Find(hash);
    if (id) {
      PERFETTO_DCHECK(Get(*id) == str);
      return *id;
    }
    return std::nullopt;
  }

  NullTermStringView Get(Id id) const {
    if (id.is_null())
      return NullTermStringView();
    if (id.is_large_string())
      return GetLargeString(id);
    return GetFromBlockPtr(IdToPtr(id));
  }

  Iterator CreateIterator() const { return Iterator(this); }

  size_t size() const { return string_index_.size(); }

  // Maximum Id of a small (not large) string in the string pool.
  StringPool::Id MaxSmallStringId() const {
    return Id::BlockString(blocks_.size() - 1, blocks_.back().pos());
  }

  // Returns whether there is at least one large string in a string pool
  bool HasLargeString() const { return !large_strings_.empty(); }

 private:
  using StringHash = uint64_t;

  struct Block {
    explicit Block(size_t size)
        : mem_(base::PagedMemory::Allocate(size,
                                           base::PagedMemory::kDontCommit)),
          size_(size) {}
    ~Block() = default;

    // Allow std::move().
    Block(Block&&) noexcept = default;
    Block& operator=(Block&&) = default;

    // Disable implicit copy.
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    uint8_t* Get(uint32_t offset) const {
      return static_cast<uint8_t*>(mem_.Get()) + offset;
    }

    std::pair<bool /*success*/, uint32_t /*offset*/> TryInsert(
        base::StringView str);

    uint32_t OffsetOf(const uint8_t* ptr) const {
      PERFETTO_DCHECK(Get(0) < ptr &&
                      ptr <= Get(static_cast<uint32_t>(size_ - 1)));
      return static_cast<uint32_t>(ptr - Get(0));
    }

    uint32_t pos() const { return pos_; }

   private:
    base::PagedMemory mem_;
    uint32_t pos_ = 0;
    size_t size_ = 0;
  };

  friend class Iterator;
  friend class StringPoolTest;

  // StringPool IDs are 32-bit. If the MSB is 1, the remaining bits of the ID
  // are an index into the |large_strings_| vector. Otherwise, the next 6 bits
  // are the index of the Block in the pool, and the remaining 25 bits the
  // offset of the encoded string inside the pool.
  //
  // [31] [30:25] [24:0]
  //  |      |       |
  //  |      |       +---- offset in block (or LSB of large string index).
  //  |      +------------ block index (or MSB of large string index).
  //  +------------------- 1: large string, 0: string in a Block.
  static constexpr size_t kNumBlockIndexBits = 6;
  static constexpr size_t kNumBlockOffsetBits = 25;

  static constexpr size_t kLargeStringFlagBitMask = 1u << 31;
  static constexpr size_t kBlockOffsetBitMask = (1u << kNumBlockOffsetBits) - 1;
  static constexpr size_t kBlockIndexBitMask =
      0xffffffff & ~kLargeStringFlagBitMask & ~kBlockOffsetBitMask;

  static constexpr size_t kBlockSizeBytes = kBlockOffsetBitMask + 1;  // 32 MB

  // If a string doesn't fit into the current block, we can either start a new
  // block or insert the string into the |large_strings_| vector. To maximize
  // the used proportion of each block's memory, we only start a new block if
  // the string isn't very large.
  static constexpr size_t kMinLargeStringSizeBytes = kBlockSizeBytes / 8;

  // Number of bytes to reserve for size and null terminator.
  // This is the upper limit on metadata size: 5 bytes for max uint32,
  // plus 1 byte for null terminator. The actual size may be lower.
  static constexpr uint8_t kMaxMetadataSize = 6;

  // Inserts the string with the given hash into the pool and return its Id.
  Id InsertString(base::StringView, uint64_t hash);

  // Insert a large string into the pool and return its Id.
  Id InsertLargeString(base::StringView, uint64_t hash);

  // The returned pointer points to the start of the string metadata (i.e. the
  // first byte of the size).
  const uint8_t* IdToPtr(Id id) const {
    // If the MSB is set, the ID represents an index into |large_strings_|, so
    // shouldn't be converted into a block pointer.
    PERFETTO_DCHECK(!id.is_large_string());

    size_t block_index = id.block_index();
    uint32_t block_offset = id.block_offset();

    PERFETTO_DCHECK(block_index < blocks_.size());
    PERFETTO_DCHECK(block_offset < blocks_[block_index].pos());

    return blocks_[block_index].Get(block_offset);
  }

  // |ptr| should point to the start of the string metadata (i.e. the first byte
  // of the size).
  // Returns pointer to the start of the string.
  static const uint8_t* ReadSize(const uint8_t* ptr, uint32_t* size) {
    uint64_t value = 0;
    const uint8_t* str_ptr = protozero::proto_utils::ParseVarInt(
        ptr, ptr + kMaxMetadataSize, &value);
    PERFETTO_DCHECK(str_ptr != ptr);
    PERFETTO_DCHECK(value < std::numeric_limits<uint32_t>::max());
    *size = static_cast<uint32_t>(value);
    return str_ptr;
  }

  // |ptr| should point to the start of the string metadata (i.e. the first byte
  // of the size).
  static NullTermStringView GetFromBlockPtr(const uint8_t* ptr) {
    uint32_t size = 0;
    const uint8_t* str_ptr = ReadSize(ptr, &size);
    return {reinterpret_cast<const char*>(str_ptr), size};
  }

  // Lookup a string in the |large_strings_| vector. |id| should have the MSB
  // set.
  NullTermStringView GetLargeString(Id id) const {
    PERFETTO_DCHECK(id.is_large_string());
    size_t index = id.large_string_index();
    PERFETTO_DCHECK(index < large_strings_.size());
    const std::string* str = large_strings_[index].get();
    return {str->c_str(), str->size()};
  }

  // The actual memory storing the strings.
  std::vector<Block> blocks_;

  // Any string that is too large to fit into a Block is stored separately
  // (inside a unique_ptr to ensure any references to it remain valid even if
  // |large_strings_| is resized).
  std::vector<std::unique_ptr<std::string>> large_strings_;

  // Maps hashes of strings to the Id in the string pool.
  base::FlatHashMap<StringHash,
                    Id,
                    base::AlreadyHashed<StringHash>,
                    base::LinearProbe,
                    /*AppendOnly=*/true>
      string_index_{/*initial_capacity=*/4096u};
};

}  // namespace perfetto::trace_processor

template <>
struct std::hash<::perfetto::trace_processor::StringPool::Id> {
  using argument_type = ::perfetto::trace_processor::StringPool::Id;
  using result_type = size_t;

  result_type operator()(const argument_type& r) const {
    return std::hash<uint32_t>{}(r.raw_id());
  }
};

#endif  // SRC_TRACE_PROCESSOR_CONTAINERS_STRING_POOL_H_
