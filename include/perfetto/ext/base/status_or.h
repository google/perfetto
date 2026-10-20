/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_STATUS_OR_H_
#define INCLUDE_PERFETTO_EXT_BASE_STATUS_OR_H_

#include <new>
#include <type_traits>
#include <utility>

#include "perfetto/base/status.h"

namespace perfetto {
namespace base {

// Union of a object of type |T| with a |base::Status|. Useful for cases where
// a |T| indicates a successful result of an operation and |base::Status|
// represents an error.
//
// This class is modelled closely on absl::Status and should essentially 1:1
// match it's API.
//
// The value is held only while the status is ok, in a union, as
// std::optional holds its value: there is no separate flag for whether there
// is a value. A value is only ever constructed where there was none and
// destroyed where there will be none; one value replaces another by T's own
// assignment.
template <typename T>
class StatusOr {
 public:
  static_assert(!std::is_const<T>::value,
                "StatusOr<const T> is not supported: use StatusOr<T>");

  // Matches naming of declarations in similar types e.g. std::optional,
  // std::variant.
  using value_type = T;

  // Intentionally implicit to allow idomatic usage (e.g. returning value/status
  // from base::StatusOr returning function).
  StatusOr(base::Status status) : status_(std::move(status)) {
    if (status_.ok()) {
      // Matches what Abseil's approach towards OkStatus being passed to
      // absl::StatusOr<T>.
      PERFETTO_FATAL("base::OkStatus passed to StatusOr: this is not allowed");
    }
  }
  StatusOr(T value) { new (&value_) T(std::move(value)); }

  StatusOr(const StatusOr& other) : status_(other.status_) {
    if (ok()) {
      new (&value_) T(other.value_);
    }
  }
  // A moved-from StatusOr keeps its status, and so a moved-from value which
  // it still destroys.
  StatusOr(StatusOr&& other) noexcept(
      std::is_nothrow_move_constructible<T>::value) {
    if (other.ok()) {
      new (&value_) T(std::move(other.value_));
    } else {
      status_ = std::move(other.status_);
    }
  }
  StatusOr& operator=(const StatusOr& other) {
    if (this != &other) {
      if (ok() && other.ok()) {
        value_ = other.value_;
      } else {
        Destroy();
        status_ = other.status_;
        if (ok()) {
          new (&value_) T(other.value_);
        }
      }
    }
    return *this;
  }
  StatusOr& operator=(StatusOr&& other) noexcept(
      std::is_nothrow_move_assignable<T>::value &&
      std::is_nothrow_move_constructible<T>::value) {
    if (this != &other) {
      if (ok() && other.ok()) {
        value_ = std::move(other.value_);
      } else if (other.ok()) {
        status_ = base::OkStatus();
        new (&value_) T(std::move(other.value_));
      } else {
        Destroy();
        status_ = std::move(other.status_);
      }
    }
    return *this;
  }
  ~StatusOr() { Destroy(); }

  bool ok() const { return status_.ok(); }
  const base::Status& status() const& { return status_; }
  // Moves an error out, leaving the value of an ok StatusOr in place.
  base::Status status() && {
    return ok() ? base::OkStatus() : std::move(status_);
  }

  // For an lvalue caller, if ok(), returns a reference to the value, so the
  // StatusOr keeps owning it.
  // If not, we log the error message and abort.
  T& ValueOrDie() & {
    if (!ok())
      PERFETTO_FATAL("%s", status_.c_message());
    return value_;
  }

  // For an rvalue caller, if ok(), moves the value out because the StatusOr
  // won't be used again.
  // If not, we log the error message and abort.
  T ValueOrDie() && {
    if (!ok())
      PERFETTO_FATAL("%s", status_.c_message());
    return std::move(value_);
  }

  T& value() {
    PERFETTO_DCHECK(status_.ok());
    return value_;
  }
  const T& value() const { return value_; }

  T& operator*() { return value(); }
  const T& operator*() const { return value(); }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

 private:
  // Ends the value's lifetime, if there is one.
  void Destroy() {
    if (ok()) {
      value_.~T();
    }
  }

  base::Status status_;
  union {
    T value_;
  };
};

// Deduction guide to make returning StatusOr less verbose.
template <typename T>
StatusOr(T) -> StatusOr<T>;

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_STATUS_OR_H_
