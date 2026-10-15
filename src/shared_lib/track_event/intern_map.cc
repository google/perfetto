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

#include "src/shared_lib/track_event/intern_map.h"

namespace perfetto {
InternMap::InternMap() = default;

InternMap::~InternMap() = default;

InternMap::FindOrAssignRes InternMap::FindOrAssign(int32_t type,
                                                   const void* value,
                                                   size_t value_size) {
  // This would be more efficient with heterogeneous lookups, but C++17 doesn't
  // support that.
  auto* it = map_.Find(Key::NonOwning(type, value, value_size));
  if (it != nullptr) {
    return {*it, false};
  }
  uint64_t iid = ++last_iid_by_type_[type];
  it = map_.Insert(Key::Owning(type, value, value_size), iid).first;
  return {iid, true};
}

InternMap::FindOrAssignRes InternMap::FindOrAssignStringSlow(
    StringCacheEntry* set,
    int32_t type,
    const char* str) {
  const size_t len = strlen(str);
  FindOrAssignRes res = FindOrAssign(type, str, len);
  if (len < sizeof(set[0].str)) {
    // The new string takes the first way, and the previous first way moves to
    // the second one, so the set keeps the two most recent misses.
    static_assert(kStringCacheWays == 2, "Update the replacement below");
    set[1] = set[0];
    set[0].ptr = str;
    set[0].iid = res.iid;
    set[0].type = type;
    memcpy(set[0].str, str, len + 1);
  }
  return res;
}

}  // namespace perfetto
