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

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/no_destructor.h"

namespace perfetto::base {

struct Status::Rep {
  struct Payload {
    std::string type_url;
    std::string payload;
  };
  std::string message;
  std::vector<Payload> payloads;
};

Status::Status(std::string msg) {
  PERFETTO_CHECK(!msg.empty());
  rep_ = reinterpret_cast<uintptr_t>(new Rep{std::move(msg), {}});
}

void Status::Delete(uintptr_t rep) {
  delete reinterpret_cast<Rep*>(rep);
}

uintptr_t Status::Clone(uintptr_t rep) {
  return reinterpret_cast<uintptr_t>(new Rep(*reinterpret_cast<Rep*>(rep)));
}

// The status's Rep, or null if it has none.
Status::Rep* Status::rep() const {
  return rep_ > kMovedFrom ? reinterpret_cast<Rep*>(rep_) : nullptr;
}

const std::string& Status::message() const {
  static NoDestructor<std::string> empty;
  static NoDestructor<std::string> moved_from("Status accessed after move");
  if (rep_ == kOk) {
    return empty.ref();
  }
  return rep_ == kMovedFrom ? moved_from.ref() : rep()->message;
}

Status ErrStatus(const char* format, ...) {
  std::string buf;
  buf.resize(1024);
  for (;;) {
    va_list ap;
    va_start(ap, format);
    int N = vsnprintf(buf.data(), buf.size() - 1, format, ap);
    va_end(ap);

    if (N <= 0) {
      buf = "[printf format error]";
      break;
    }

    auto sN = static_cast<size_t>(N);
    if (sN > buf.size() - 1) {
      // Indicates that the string was truncated and sN is the "number of
      // non-null bytes which would be needed to fit the result". This is the
      // C99 standard behaviour in the case of truncation. In that case, resize
      // the buffer to match the returned value (with + 1 for the null
      // terminator) and try again.
      buf.resize(sN + 1);
      continue;
    }
    if (sN == buf.size() - 1) {
      // Indicates that the string was likely truncated and sN is just the
      // number of bytes written into the string. This is the behaviour of
      // non-standard compilers (MSVC) etc. In that case, just double the
      // storage and try again.
      buf.resize(sN * 2);
      continue;
    }

    // Otherwise, indicates the string was written successfully: we need to
    // resize to match the number of non-null bytes and return.
    buf.resize(sN);
    break;
  }
  return Status(std::move(buf));
}

std::optional<std::string_view> Status::GetPayload(
    std::string_view type_url) const {
  if (!rep()) {
    return std::nullopt;
  }
  for (const auto& kv : rep()->payloads) {
    if (kv.type_url == type_url) {
      return kv.payload;
    }
  }
  return std::nullopt;
}

void Status::SetPayload(std::string_view type_url, std::string value) {
  if (!rep()) {
    return;
  }
  for (auto& kv : rep()->payloads) {
    if (kv.type_url == type_url) {
      kv.payload = value;
      return;
    }
  }
  rep()->payloads.push_back(
      Rep::Payload{std::string(type_url), std::move(value)});
}

bool Status::ErasePayload(std::string_view type_url) {
  if (!rep()) {
    return false;
  }
  auto& payloads = rep()->payloads;
  auto it = std::remove_if(
      payloads.begin(), payloads.end(),
      [type_url](const Rep::Payload& p) { return p.type_url == type_url; });
  bool erased = it != payloads.end();
  payloads.erase(it, payloads.end());
  return erased;
}

}  // namespace perfetto::base
