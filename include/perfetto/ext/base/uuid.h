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

#ifndef INCLUDE_PERFETTO_EXT_BASE_UUID_H_
#define INCLUDE_PERFETTO_EXT_BASE_UUID_H_

#include <array>
#include <string>

#include "perfetto/ext/base/optional.h"

namespace perfetto {
namespace base {

using Uuid = std::array<uint8_t, 16>;
Uuid Uuidv4();

Uuid StringToUuid(const std::string&);
std::string UuidToString(const Uuid&);
std::string UuidToPrettyString(const Uuid&);
Optional<Uuid> BytesToUuid(const uint8_t* data, size_t size);
inline Optional<Uuid> BytesToUuid(const char* data, size_t size) {
  return BytesToUuid(reinterpret_cast<const uint8_t*>(data), size);
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_UUID_H_
