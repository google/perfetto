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

#ifndef SRC_TRACE_PROCESSOR_STRING_POOL_H_
#define SRC_TRACE_PROCESSOR_STRING_POOL_H_

#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/trace_processor/null_term_string_view.h"

#include <unordered_map>
#include <vector>

namespace perfetto {
namespace trace_processor {

// On 64-bit platforms, the string pool is implemented as a mmaped buffer
// of 4GB with the id being equal ot the offset into this buffer of the string.
// On 32-bit platforms instead, the implementation allocates 32MB blocks of
// mmaped memory with the pointer being directly converted to the id.
constexpr size_t kDefaultBlockSize =
    sizeof(void*) == 8
        ? static_cast<size_t>(4ull * 1024ull * 1024ull * 1024ull) /* 4GB */
        : 32ull * 1024ull * 1024ull /* 32MB */;

// Interns strings in a string pool and hands out compact StringIds which can
// be used to retrieve the string in O(1).
class StringPool {
 public:
  using Id = uint32_t;

  // Iterator over the strings in the pool.
  class Iterator {
   public:
    Iterator(const StringPool*);

    explicit operator bool() const;
    Iterator& operator++();

    NullTermStringView StringView();
    Id StringId();

   private:
    const StringPool* pool_ = nullptr;
    uint32_t block_id_ = 0;
    uint32_t block_offset_ = 0;
  };

  StringPool();
  ~StringPool();

  // Allow std::move().
  StringPool(StringPool&&) noexcept;
  StringPool& operator=(StringPool&&);

  // Disable implicit copy.
  StringPool(const StringPool&) = delete;
  StringPool& operator=(const StringPool&) = delete;

  Id InternString(base::StringView str) {
    if (str.data() == nullptr)
      return 0;

    auto hash = str.Hash();
    auto id_it = string_index_.find(hash);
    if (id_it != string_index_.end()) {
      PERFETTO_DCHECK(Get(id_it->second) == str);
      return id_it->second;
    }
    return InsertString(str, hash);
  }

  base::Optional<Id> GetId(base::StringView str) const {
    if (str.data() == nullptr)
      return 0u;

    auto hash = str.Hash();
    auto id_it = string_index_.find(hash);
    if (id_it != string_index_.end()) {
      PERFETTO_DCHECK(Get(id_it->second) == str);
      return id_it->second;
    }
    return base::nullopt;
  }

  NullTermStringView Get(Id id) const {
    if (id == 0)
      return NullTermStringView();
    return GetFromPtr(IdToPtr(id));
  }

  Iterator CreateIterator() const { return Iterator(this); }

  size_t size() const { return string_index_.size(); }

 private:
  using StringHash = uint64_t;

  struct Block {
    explicit Block(size_t size)
        : mem_(base::PagedMemory::Allocate(size)), size_(size) {}
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

    const uint8_t* TryInsert(base::StringView str);

    uint32_t OffsetOf(const uint8_t* ptr) const {
      PERFETTO_DCHECK(Get(0) < ptr &&
                      ptr < Get(static_cast<uint32_t>(size_ - 1)));
      return static_cast<uint32_t>(ptr - Get(0));
    }

    uint32_t pos() const { return pos_; }

   private:
    base::PagedMemory mem_;
    uint32_t pos_ = 0;
    size_t size_;
  };

  friend class Iterator;

  // Number of bytes to reserve for size and null terminator.
  // This is the upper limit on metadata size: 5 bytes for max uint32,
  // plus 1 byte for null terminator. The actual size may be lower.
  static constexpr uint8_t kMaxMetadataSize = 6;

  // Inserts the string with the given hash into the pool
  Id InsertString(base::StringView, uint64_t hash);

  // |ptr| should point to the start of the string metadata (i.e. the first byte
  // of the size).
  Id PtrToId(const uint8_t* ptr) const {
    // For a 64 bit architecture, the id is the offset of the pointer inside
    // the one and only 4GB block.
    if (sizeof(void*) == 8) {
      PERFETTO_DCHECK(blocks_.size() == 1);
      return blocks_.back().OffsetOf(ptr);
    }

    // On 32 bit architectures, the size of the pointer is 32-bit so we simply
    // use the pointer itself as the id.
    // Double cast needed because, on 64 archs, the compiler complains that we
    // are losing information.
    return static_cast<Id>(reinterpret_cast<uintptr_t>(ptr));
  }

  // The returned pointer points to the start of the string metadata (i.e. the
  // first byte of the size).
  const uint8_t* IdToPtr(Id id) const {
    // For a 64 bit architecture, the pointer is simply the found by taking
    // the base of the 4GB block and adding the offset given by |id|.
    if (sizeof(void*) == 8) {
      PERFETTO_DCHECK(blocks_.size() == 1);
      return blocks_.back().Get(id);
    }
    // On a 32 bit architecture, the pointer is the same as the id.
    return reinterpret_cast<uint8_t*>(id);
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
  static NullTermStringView GetFromPtr(const uint8_t* ptr) {
    uint32_t size = 0;
    const uint8_t* str_ptr = ReadSize(ptr, &size);
    return NullTermStringView(reinterpret_cast<const char*>(str_ptr), size);
  }

  // The actual memory storing the strings.
  std::vector<Block> blocks_;

  // Maps hashes of strings to the Id in the string pool.
  // TODO(lalitm): At some point we should benchmark just using a static
  // hashtable of 1M elements, we can afford paying a fixed 8MB here
  std::unordered_map<StringHash, Id> string_index_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_STRING_POOL_H_
