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

#ifndef INCLUDE_PERFETTO_BASE_STATUS_H_
#define INCLUDE_PERFETTO_BASE_STATUS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/export.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace base {

// Represents either the success or the failure message of a function.
// This can used as the return type of functions which would usually return an
// bool for success or int for errno but also wants to add some string context
// (ususally for logging).
//
// Similar to absl::Status, an optional "payload" can also be included with more
// context about the error. This allows passing additional metadata about the
// error (e.g. location of errors, potential mitigations etc).
//
// A Status is one word: zero when ok, else the error, kept on the heap. Errors
// are rare and statuses are created, moved and destroyed everywhere, so all
// an ok status costs is inline and trivial, and everything to do with an
// error is out of line.
class PERFETTO_EXPORT_COMPONENT Status {
 public:
  constexpr Status() = default;
  explicit Status(std::string msg);

  Status(const Status& other)
      : rep_(other.rep_ > kMovedFrom ? Clone(other.rep_) : other.rep_) {}
  Status& operator=(const Status& other) {
    if (this != &other) {
      Status copy(other);
      std::swap(rep_, copy.rep_);
    }
    return *this;
  }

  // Move operations. A moved-from status is an error saying so, as accessing
  // it is a bug, rather than passing for ok.
  Status(Status&& other) noexcept
      : rep_(std::exchange(other.rep_, kMovedFrom)) {}
  Status& operator=(Status&& other) noexcept {
    if (this != &other) {
      Release();
      rep_ = std::exchange(other.rep_, kMovedFrom);
    }
    return *this;
  }

  ~Status() { Release(); }

  bool ok() const { return rep_ == kOk; }

  // When ok() is false this returns the error message. Returns the empty string
  // otherwise.
  const std::string& message() const;
  const char* c_message() const { return message().c_str(); }

  //////////////////////////////////////////////////////////////////////////////
  // Payload Management APIs
  //////////////////////////////////////////////////////////////////////////////

  // Payloads can be attached to error statuses to provide additional context.
  //
  // Payloads are (key, value) pairs, where the key is a string acting as a
  // unique "type URL" and the value is an opaque string. The "type URL" should
  // be unique, follow the format of a URL and, ideally, documentation on how to
  // interpret its associated data should be available.
  //
  // To attach a payload to a status object, call `Status::SetPayload()`.
  // Similarly, to extract the payload from a status, call
  // `Status::GetPayload()`.
  //
  // Note: the payload APIs are only meaningful to call when the status is an
  // error. Otherwise, all methods are noops.

  // Gets the payload for the given |type_url| if one exists.
  //
  // Will always return std::nullopt if |ok()|.
  std::optional<std::string_view> GetPayload(std::string_view type_url) const;

  // Sets the payload for the given key. The key should
  //
  // Will always do nothing if |ok()|.
  void SetPayload(std::string_view type_url, std::string value);

  // Erases the payload for the given string and returns true if the payload
  // existed and was erased.
  //
  // Will always do nothing if |ok()|.
  bool ErasePayload(std::string_view type_url);

 private:
  // The error: its message and payloads.
  struct Rep;

  // Values of `rep_` which are not a Rep.
  static constexpr uintptr_t kOk = 0;
  static constexpr uintptr_t kMovedFrom = 1;

  void Release() {
    if (PERFETTO_UNLIKELY(rep_ > kMovedFrom)) {
      Delete(rep_);
    }
  }
  static void Delete(uintptr_t rep);
  static uintptr_t Clone(uintptr_t rep);
  Rep* rep() const;

  uintptr_t rep_ = kOk;
};

// Returns a status object which represents the Ok status.
inline Status OkStatus() {
  return Status();
}

Status ErrStatus(const char* format, ...) PERFETTO_PRINTF_FORMAT(1, 2);

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_STATUS_H_
