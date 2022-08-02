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

#include <string>

#include "perfetto/base/compiler.h"
#include "perfetto/base/export.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace base {

// Represents either the success or the failure message of a function.
// This can used as the return type of functions which would usually return an
// bool for success or int for errno but also wants to add some string context
// (ususally for logging).
class PERFETTO_EXPORT_COMPONENT Status {
 public:
  Status() : ok_(true) {}
  explicit Status(std::string msg) : ok_(false), message_(std::move(msg)) {
    PERFETTO_CHECK(!message_.empty());
  }

  // Copy operations.
  Status(const Status&) = default;
  Status& operator=(const Status&) = default;

  // Move operations. The moved-from state is valid but unspecified.
  Status(Status&&) noexcept = default;
  Status& operator=(Status&&) = default;

  bool ok() const { return ok_; }

  // When ok() is false this returns the error message. Returns the empty string
  // otherwise.
  const std::string& message() const { return message_; }
  const char* c_message() const { return message_.c_str(); }

 private:
  bool ok_ = false;
  std::string message_;
};

// Returns a status object which represents the Ok status.
inline Status OkStatus() {
  return Status();
}

PERFETTO_PRINTF_FORMAT(1, 2) Status ErrStatus(const char* format, ...);

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_STATUS_H_
