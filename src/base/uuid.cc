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

#include "perfetto/ext/base/uuid.h"

#include <random>

#include "perfetto/base/time.h"

namespace perfetto {
namespace base {
namespace {

constexpr char kHexmap[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                            '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
}  // namespace

// See https://www.ietf.org/rfc/rfc4122.txt
Uuid Uuidv4() {
  static std::minstd_rand rng(static_cast<uint32_t>(GetBootTimeNs().count()));
  Uuid uuid;
  for (size_t i = 0; i < 16; ++i)
    uuid[i] = static_cast<uint8_t>(rng());

  // version:
  uuid[6] = (uuid[6] & 0x0f) | 0x40;
  // clock_seq_hi_and_reserved:
  uuid[8] = (uuid[8] & 0x3f) | 0x80;

  return uuid;
}

std::string UuidToString(const Uuid& uuid) {
  return std::string(reinterpret_cast<const char*>(uuid.data()), uuid.size());
}

std::string UuidToPrettyString(const Uuid& uuid) {
  std::string s(uuid.size() * 2 + 4, '-');
  // Format is 123e4567-e89b-12d3-a456-426655443322.
  size_t j = 0;
  for (size_t i = 0; i < uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      j++;
    s[2 * i + j] = kHexmap[(uuid[i] & 0xf0) >> 4];
    s[2 * i + 1 + j] = kHexmap[(uuid[i] & 0x0f)];
  }
  return s;
}

Uuid StringToUuid(const std::string& s) {
  Uuid uuid;
  PERFETTO_CHECK(s.size() == uuid.size());
  for (size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<uint8_t>(s[i]);
  }
  return uuid;
}

Optional<Uuid> BytesToUuid(const uint8_t* data, size_t size) {
  Uuid uuid;
  if (size != uuid.size())
    return nullopt;
  for (size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = data[i];
  }
  return uuid;
}

}  // namespace base
}  // namespace perfetto
