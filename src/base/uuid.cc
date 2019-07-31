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

#include "perfetto/ext/base/time.h"

namespace perfetto {
namespace base {

// See https://www.ietf.org/rfc/rfc4122.txt
std::array<uint8_t, 16> Uuidv4() {
  static std::minstd_rand rng(static_cast<size_t>(GetBootTimeNs().count()));
  std::array<uint8_t, 16> uuid;
  for (size_t i = 0; i < 16; ++i)
    uuid[i] = static_cast<uint8_t>(rng());

  // version:
  uuid[6] = (uuid[6] & 0x0f) | 0x40;
  // clock_seq_hi_and_reserved:
  uuid[8] = (uuid[8] & 0x3f) | 0x80;

  return uuid;
}

}  // namespace base
}  // namespace perfetto
