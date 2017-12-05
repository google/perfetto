/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PROTOZERO_PROTOZERO_MESSAGE_HANDLE_H_
#define INCLUDE_PERFETTO_PROTOZERO_PROTOZERO_MESSAGE_HANDLE_H_

#include <functional>

#if defined(NDEBUG) && !defined(DCHECK_ALWAYS_ON)
#define PROTOZERO_ENABLE_HANDLE_DEBUGGING() 0
#else
#define PROTOZERO_ENABLE_HANDLE_DEBUGGING() 1
#endif

namespace protozero {

class ProtoZeroMessage;

// ProtoZeroMessageHandle allows to decouple the lifetime of a proto message
// from the underlying storage. It gives the following guarantees:
// - The underlying message is finalized (if still alive) if the handle goes
//   out of scope.
// - In Debug / DCHECK_ALWAYS_ON builds, the handle becomes null once the
//   message is finalized. This is to enforce the append-only API. For instance
//   when adding two repeated messages, the addition of the 2nd one forces
//   the finalization of the first.
// Think about this as a WeakPtr<ProtoZeroMessage> which calls
// ProtoZeroMessage::Finalize() when going out of scope.

class ProtoZeroMessageHandleBase {
 public:
  ~ProtoZeroMessageHandleBase();

  // Move-only type.
  ProtoZeroMessageHandleBase(ProtoZeroMessageHandleBase&&) noexcept;
  ProtoZeroMessageHandleBase& operator=(ProtoZeroMessageHandleBase&&);

  void Finalize();
  void set_on_finalize(std::function<void(size_t)> f) { on_finalize_ = f; }

 protected:
  explicit ProtoZeroMessageHandleBase(ProtoZeroMessage* = nullptr);
  ProtoZeroMessage& operator*() const { return *message_; }
  ProtoZeroMessage* operator->() const { return message_; }

 private:
  friend class ProtoZeroMessage;
  ProtoZeroMessageHandleBase(const ProtoZeroMessageHandleBase&) = delete;
  ProtoZeroMessageHandleBase& operator=(const ProtoZeroMessageHandleBase&) =
      delete;

  void reset_message() { message_ = nullptr; }
  void Move(ProtoZeroMessageHandleBase&&);

  ProtoZeroMessage* message_;
  std::function<void(size_t)> on_finalize_;
};

template <typename T>
class ProtoZeroMessageHandle : public ProtoZeroMessageHandleBase {
 public:
  ProtoZeroMessageHandle() : ProtoZeroMessageHandle(nullptr) {}
  explicit ProtoZeroMessageHandle(T* message)
      : ProtoZeroMessageHandleBase(message) {}

  T& operator*() const {
    return static_cast<T&>(ProtoZeroMessageHandleBase::operator*());
  }

  T* operator->() const {
    return static_cast<T*>(ProtoZeroMessageHandleBase::operator->());
  }
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_PROTOZERO_MESSAGE_HANDLE_H_
