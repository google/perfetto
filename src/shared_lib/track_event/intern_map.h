/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_SHARED_LIB_TRACK_EVENT_INTERN_MAP_H_
#define SRC_SHARED_LIB_TRACK_EVENT_INTERN_MAP_H_

#include <array>
#include <cstdint>
#include <cstring>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/public/fnv1a.h"

namespace perfetto {

// Assigns and maintains the mapping between "interned" data and iids (small
// integers that can be used to refer to the same data without repeating it)
// for different types.
class InternMap {
 public:
  // Zero will never be assigned as a valid iid: it is used as the return value
  // of Find() to signal failure.
  using Iid = uint64_t;

  InternMap();
  ~InternMap();

  // Clears all interned data, resetting the map to its initial state.
  // This allows reusing allocated memory instead of destroying and recreating.
  void Clear() {
    map_.Clear();
    last_iid_by_type_.Clear();
    string_cache_.fill(StringCacheEntry());
  }

  // Given a value (identified by the memory buffer starting at `value`,
  // `value_size` bytes long) of a specific `type`, finds if there was an
  // existing iid associated with it, or assigns a new iid to it. Assigned iids
  // are unique for a specific type, but are reused across different types.
  struct FindOrAssignRes {
    // Iid associated with the passed value.
    Iid iid;
    // Whether the iid was newly assigned in this call (i.e. true if the value
    // was not seen before).
    bool newly_assigned;
  };
  FindOrAssignRes FindOrAssign(int32_t type,
                               const void* value,
                               size_t value_size);

  // Same as FindOrAssign(type, str, strlen(str)), for a NUL-terminated `str`.
  //
  // Track events intern their name and their debug annotation names on every
  // event, and most events reuse the same few strings. A small cache keyed by
  // the string pointer answers those without hashing the string.
  // A hit also compares the content, so a buffer reused for a different
  // string misses.
  FindOrAssignRes FindOrAssignString(int32_t type, const char* str) {
    StringCacheEntry* set = StringCacheSet(type, str);
    for (size_t way = 0; way < kStringCacheWays; way++) {
      if (set[way].Matches(type, str))
        return {set[way].iid, false};
    }
    return FindOrAssignStringSlow(set, type, str);
  }

 private:
  // One string in the cache of FindOrAssignString().
  struct StringCacheEntry {
    bool Matches(int32_t t, const char* s) const {
      return ptr == s && type == t && strcmp(s, str) == 0;
    }

    const char* ptr = nullptr;
    Iid iid = 0;
    int32_t type = 0;
    // A copy of the content. Only strings shorter than this are cached.
    char str[28] = {};
  };

  // The cache is 2-way set associative: each set holds the last two strings
  // whose pointer maps to it.
  static constexpr size_t kStringCacheSets = 32;
  static constexpr size_t kStringCacheWays = 2;

  StringCacheEntry* StringCacheSet(int32_t type, const char* str) {
    const uint64_t key =
        reinterpret_cast<uintptr_t>(str) ^ static_cast<uint32_t>(type);
    const size_t set = base::MurmurHashValue(key) % kStringCacheSets;
    return &string_cache_[set * kStringCacheWays];
  }

  FindOrAssignRes FindOrAssignStringSlow(StringCacheEntry* set,
                                         int32_t type,
                                         const char* str);

  // Stores a value of a specific type. If the value is small, it is stored
  // inline, otherwise it is stored in an external buffer. The Key can own the
  // external buffer (when the key is stored in the map) or not (when the key is
  // just used for lookup).
  class Key {
   public:
    static Key NonOwning(int32_t type, const void* value, size_t value_size) {
      Key key;
      key.type_ = type;
      key.val_type_ = ValueStorage::kNonOwnedPtr;
      key.ptr_ = value;
      key.value_size_ = value_size;
      return key;
    }
    static Key Owning(int32_t type, const void* value, size_t value_size) {
      Key key;
      key.type_ = type;
      key.value_size_ = value_size;
      if (value_size < sizeof(value_buf_)) {
        key.val_type_ = ValueStorage::kInline;
        memcpy(key.value_buf_, value, value_size);
      } else {
        key.val_type_ = ValueStorage::kOwnedPtr;
        char* newvalue = new char[value_size];
        memcpy(newvalue, value, value_size);
        key.owned_ptr_ = newvalue;
      }
      return key;
    }
    ~Key() {
      if (val_type_ == ValueStorage::kOwnedPtr) {
        delete[] owned_ptr_;
      }
    }
    Key(const Key&) = delete;
    Key(Key&& other) noexcept {
      type_ = other.type_;
      value_size_ = other.value_size_;
      switch (other.val_type_) {
        case ValueStorage::kNonOwnedPtr:
          val_type_ = ValueStorage::kNonOwnedPtr;
          ptr_ = other.ptr_;
          return;
        case ValueStorage::kOwnedPtr:
          val_type_ = ValueStorage::kOwnedPtr;
          owned_ptr_ = other.owned_ptr_;
          other.val_type_ = ValueStorage::kInline;
          other.value_size_ = 0;
          return;
        case ValueStorage::kInline:
          val_type_ = ValueStorage::kInline;
          memcpy(value_buf_, other.value_buf_, value_size_);
          return;
      }
    }
    bool operator==(const Key& other) const {
      if (type_ != other.type_) {
        return false;
      }
      if (value_size_ != other.value_size_) {
        return false;
      }
      return memcmp(value(), other.value(), value_size_) == 0;
    }
    const void* value() const {
      switch (val_type_) {
        case ValueStorage::kNonOwnedPtr:
          return ptr_;
        case ValueStorage::kOwnedPtr:
          return owned_ptr_;
        case ValueStorage::kInline:
          break;
      }
      return &value_buf_;
    }
    struct Hash {
      size_t operator()(const Key& obj) const {
        return std::hash<int32_t>()(obj.type_) ^
               static_cast<size_t>(PerfettoFnv1a(obj.value(), obj.value_size_));
      }
    };

   private:
    enum class ValueStorage {
      kInline,       // `value_buf_` is used directly to store the value.
      kNonOwnedPtr,  // `ptr_` points to an external buffer that stores the
                     // value. Not owned by this Key.
      kOwnedPtr,     // `owned_ptr_` points to an external buffer that stores
                     // the value. Owned by this Key.
    };
    Key() = default;
    int32_t type_;
    ValueStorage val_type_;
    size_t value_size_;
    union {
      char value_buf_[sizeof(uint64_t)];
      const void* ptr_;
      char* owned_ptr_;
    };
  };

  base::FlatHashMap<Key, uint64_t, Key::Hash> map_;
  base::FlatHashMap<int32_t, uint64_t> last_iid_by_type_;
  std::array<StringCacheEntry, kStringCacheSets * kStringCacheWays>
      string_cache_;
};

}  // namespace perfetto

#endif  // SRC_SHARED_LIB_TRACK_EVENT_INTERN_MAP_H_
