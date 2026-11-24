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
#include <cstdint>
#include <string>

#include "perfetto/base/endian.h"
#include "perfetto/base/export.h"

namespace perfetto {
namespace base {

class PERFETTO_EXPORT_COMPONENT Uuid {
 public:
  explicit Uuid(const std::string& s);
  explicit Uuid(int64_t lsb, int64_t msb);
  Uuid();

  std::array<uint8_t, 16>* data() { return &data_; }
  const std::array<uint8_t, 16>* data() const { return &data_; }

  bool operator==(const Uuid& other) const { return data_ == other.data_; }

  bool operator!=(const Uuid& other) const { return !(*this == other); }

  explicit operator bool() const { return *this != Uuid(); }

  int64_t msb() const { return LoadLE<int64_t>(data_.data() + 8); }

  int64_t lsb() const { return LoadLE<int64_t>(data_.data()); }

  void set_lsb_msb(int64_t lsb, int64_t msb) {
    set_lsb(lsb);
    set_msb(msb);
  }
  void set_msb(int64_t msb) { StoreLE(data_.data() + 8, msb); }
  void set_lsb(int64_t lsb) { StoreLE(data_.data(), lsb); }

  std::string ToString() const;
  std::string ToPrettyString() const;

 private:
  std::array<uint8_t, 16> data_{};
};

Uuid Uuidv4();

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_UUID_H_
