/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_THREADING_POLL_H_
#define INCLUDE_PERFETTO_EXT_BASE_THREADING_POLL_H_

#include <variant>
#include "perfetto/base/flat_set.h"
#include "perfetto/base/platform_handle.h"
#include "perfetto/ext/base/optional.h"

namespace perfetto {
namespace base {

// Forward declarations.
class PollContext;

// "Void" type for futures: this type can be used when a Future/Stream wants
// to return no value. We cannot use void directly because it causes all sorts
// of subtle issues with templates.
struct FVoid {};

// Indicates that the Future is not ready to produce data at the moment but
// will do so at a later date.
struct PendingPollResult {};

// Return value of Future<T>::Poll.
//
// Essentially a wrapper around std::variant<T, PendingPollResult> but with
// higher level API.
template <typename T>
class FuturePollResult {
 public:
  using PollT = T;

  // Intentionally implicit to allow idiomatic returns.
  FuturePollResult(const PendingPollResult&) : inner_(PendingPollResult()) {}
  FuturePollResult(T item) noexcept : inner_(std::move(item)) {}

  // Returns whether the Future is still pending.
  bool IsPending() const {
    return std::holds_alternative<PendingPollResult>(inner_);
  }

  // The real value inside this result: requires !IsPending().
  T& item() {
    PERFETTO_DCHECK(!IsPending());
    return std::get<T>(inner_);
  }
  const T& item() const {
    PERFETTO_DCHECK(!IsPending());
    return std::get<T>(inner_);
  }

  // The real value inside this result: requires !IsPending().
  T* operator->() { return &item(); }
  const T* operator->() const { return &item(); }

 private:
  std::variant<PendingPollResult, T> inner_;
};

// Interface for implementing the Future<T>::Poll function.
//
// This is essentially a variant of the common PIMPL (pointer to impl) pattern
// used in C++ to allow having different implementations for Future<T>::Poll.
//
// We are using this instead of having an abstract function in Future to avoid
// having to wrap Future in unique_ptr everywhere it's used.
//
// We could have used std::function<Result(PollContext*)> but not all
// implementations of FuturePollable are copyable. If we had C++23, we could use
// std::move_only_function but we are some years from being able to do that.
template <typename T>
class FuturePollable {
 public:
  using PollT = T;

  virtual ~FuturePollable() = default;

  // Implementation of the Poll function of a Future: see Future documentation
  // for how this should be implemented.
  virtual FuturePollResult<T> Poll(PollContext*) = 0;
};

// Context class passed to Pollable classes.
//
// Implementations of Pollable which simply wrap another Pollable will use
// this as an opaque parameter to pass on.
//
// "Source" pollables (i.e. Pollables dealing directly with FDs) should call
// |RegisterInterested| when the FD returns EAGAIN/EWOULDBLOCK with the
// PollContext passed in.
class PollContext {
 public:
  explicit PollContext(FlatSet<PlatformHandle>* interested_handles,
                       const FlatSet<PlatformHandle>* ready_handles)
      : interested_handles_(interested_handles),
        ready_handles_(ready_handles) {}

  PollContext(PollContext&&) = default;
  PollContext& operator=(PollContext&&) = default;

  // Called by implementations of Future<T> to indicate that Poll should be
  // called again when |handle(s)| are ready for reading (or have been closed).
  void RegisterInterested(PlatformHandle handle) {
    interested_handles_->insert(handle);
  }
  void RegisterAllInterested(const FlatSet<PlatformHandle>& handles) {
    for (PlatformHandle handle : handles) {
      RegisterInterested(handle);
    }
  }

  // Returns a set of all the fds which were marked as "ready" by the operating
  // system (i.e. POLLIN/POLLHUP on Linux).
  const FlatSet<PlatformHandle>& ready_handles() const {
    return *ready_handles_;
  }

 private:
  PollContext(const PollContext&) = delete;
  PollContext& operator=(const PollContext&) = delete;

  FlatSet<PlatformHandle>* interested_handles_ = nullptr;
  const FlatSet<PlatformHandle>* ready_handles_ = nullptr;
};

// Evaluates |expr|, which should return a FuturePollResult. If IsPending is
// true, returns base::PendingPollResult().
//
// Example usage:
//
// Future<int> MyIntReturningFutureFn();
//
// FuturePollResult<std::string> Poll(PollContext* ctx) {
//   // res will be of type "int"
//   ASSIGN_OR_RETURN_IF_PENDING_FUTURE(res, MyIntReturningFutureFn());
//   return std::to_string(*foo);
// }
//
#define ASSIGN_OR_RETURN_IF_PENDING_FUTURE(var, expr) \
  auto assign_and_return_if_poll_##var = (expr);      \
  if (assign_and_return_if_poll_##var.IsPending())    \
    return base::PendingPollResult();                 \
  auto var = std::move(assign_and_return_if_poll_##var.item())

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_THREADING_POLL_H_
