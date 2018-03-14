/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACED_PROBES_FILESYSTEM_LRU_INODE_CACHE_H_
#define SRC_TRACED_PROBES_FILESYSTEM_LRU_INODE_CACHE_H_

#include <list>
#include <map>
#include <string>
#include <tuple>

namespace perfetto {
namespace base {

// LRUInodeCache keeps up to |capacity| entries in a mapping from InodeKey
// to InodeValue. This is used to map <block device, inode> tuples to file
// paths.
class LRUInodeCache {
 public:
  using InodeKey = std::pair<int64_t, int64_t>;
  using InodeValue = std::string;

  explicit LRUInodeCache(size_t capacity) : capacity_(capacity) {}

  const LRUInodeCache::InodeValue* Get(const InodeKey& k);
  void Insert(InodeKey k, LRUInodeCache::InodeValue v);

 private:
  using ItemType = std::pair<const InodeKey, const InodeValue>;
  using ListIteratorType = std::list<ItemType>::iterator;
  using MapType = std::map<const InodeKey, ListIteratorType>;

  void Insert(MapType::iterator map_it, InodeKey k, InodeValue v);

  const size_t capacity_;
  MapType map_;
  std::list<ItemType> list_;
};

}  // namespace base
}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FILESYSTEM_LRU_INODE_CACHE_H_
