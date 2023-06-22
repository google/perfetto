/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "perfetto/base/status.h"

#include <stdarg.h>
#include <algorithm>

namespace perfetto {
namespace base {

Status ErrStatus(const char* format, ...) {
  char buffer[1024];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buffer, sizeof(buffer), format, ap);
  va_end(ap);
  Status status(buffer);
  return status;
}

std::optional<std::string_view> Status::GetPayload(
    std::string_view type_url) const {
  if (ok()) {
    return std::nullopt;
  }
  for (const auto& kv : payloads_) {
    if (kv.type_url == type_url) {
      return kv.payload;
    }
  }
  return std::nullopt;
}

void Status::SetPayload(std::string_view type_url, std::string value) {
  if (ok()) {
    return;
  }
  for (auto& kv : payloads_) {
    if (kv.type_url == type_url) {
      kv.payload = value;
      return;
    }
  }
  payloads_.push_back(Payload{std::string(type_url), std::move(value)});
}

bool Status::ErasePayload(std::string_view type_url) {
  if (ok()) {
    return false;
  }
  auto it = std::remove_if(
      payloads_.begin(), payloads_.end(),
      [type_url](const Payload& p) { return p.type_url == type_url; });
  bool erased = it != payloads_.end();
  payloads_.erase(it, payloads_.end());
  return erased;
}

}  // namespace base
}  // namespace perfetto
