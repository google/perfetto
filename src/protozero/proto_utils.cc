/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/protozero/proto_utils.h"

#include <string.h>

#include <limits>

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"

namespace protozero {
namespace proto_utils {

const uint8_t* ParseVarInt(const uint8_t* start,
                           const uint8_t* end,
                           uint64_t* value) {
  const uint8_t* pos = start;
  uint64_t shift = 0;
  *value = 0;
  do {
    if (PERFETTO_UNLIKELY(pos >= end)) {
      *value = 0;
      return start;
    }
    PERFETTO_DCHECK(shift < 64ull);
    *value |= static_cast<uint64_t>(*pos & 0x7f) << shift;
    shift += 7;
  } while (*pos++ & 0x80);
  return pos;
}

}  // namespace proto_utils
}  // namespace protozero
