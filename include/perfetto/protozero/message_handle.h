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

#ifndef INCLUDE_PERFETTO_PROTOZERO_MESSAGE_HANDLE_H_
#define INCLUDE_PERFETTO_PROTOZERO_MESSAGE_HANDLE_H_

#include <type_traits>
#include <utility>

#include "perfetto/base/export.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/scattered_stream_writer.h"

namespace protozero {

class Message;

class PERFETTO_EXPORT_COMPONENT MessageFinalizationListener {
 public:
  virtual ~MessageFinalizationListener();
  virtual void OnMessageFinalized(Message* message) = 0;
};

// True if every MessageHandle<T> points to a RootMessage<T>.
// The layer that uses T as a root message provides the specialization, which
// must be visible before any use of MessageHandle<T>.
// For TracePacket use perfetto/tracing/trace_writer_base.h.
template <typename T>
struct IsRootMessage : std::false_type {};

// Non-owning handle that finalizes a protozero message on destruction.
//
// - Going out of scope or assigning a different message finalizes the old one.
// - Message::Finalize() is idempotent: calling it directly does not invalidate
//   the handle. The destructor calls it again harmlessly.
// - Destroy or clear the handle before resetting the message or releasing its
//   storage. Stale finalization would corrupt a new message at the same
//   address.
// - Holding, moving and destroying a handle do not need T to be complete. A
//   forward declaration of T is enough for that.
//
// Do not pass |kIsRoot| explicitly. See IsRootMessage.
template <typename T, bool kIsRoot = IsRootMessage<T>::value>
class MessageHandle {
 public:
  static constexpr bool kIsRootMessage = kIsRoot;

  MessageHandle() : MessageHandle(nullptr) {}

  // Creates a handle from |message|:
  // - nullptr creates an empty handle.
  // - If kIsRootMessage, the object must be a RootMessage<T>.
  // - Otherwise, the object can be any T and is finalized through Message.
  explicit MessageHandle(T* message) : message_(message) {}

  ~MessageHandle() {
    if (message_)
      FinalizeMessage();
  }

  // Move-only type.
  MessageHandle(MessageHandle&& other) noexcept { Move(std::move(other)); }

  MessageHandle& operator=(MessageHandle&& other) noexcept {
    // If the current handle was pointing to a message and is being reset to a
    // new one, finalize the old message. However, if the other message is the
    // same as the one we point to, don't finalize.
    if (message_ && message_ != other.message_)
      FinalizeMessage();
    Move(std::move(other));
    return *this;
  }

  MessageHandle(const MessageHandle&) = delete;
  MessageHandle& operator=(const MessageHandle&) = delete;

  explicit operator bool() const { return !!message_; }

  T& operator*() const { return *get(); }
  T* operator->() const { return get(); }
  T* get() const { return static_cast<T*>(message_); }

  void set_finalization_listener(MessageFinalizationListener* listener) {
    listener_ = listener;
  }

  // Returns the stream writer and clears the handle so its destructor does not
  // finalize the message or notify the listener.
  // See Message::TakeStreamWriter() for details on direct writes.
  ScatteredStreamWriter* TakeStreamWriter() {
    ScatteredStreamWriter* stream_writer = message_->TakeStreamWriter();
    message_ = nullptr;
    listener_ = nullptr;
    return stream_writer;
  }

 private:
  void Move(MessageHandle&& other) {
    message_ = std::exchange(other.message_, nullptr);
    listener_ = std::exchange(other.listener_, nullptr);
  }

  void FinalizeMessage() {
    message_->Finalize();
    if (listener_)
      listener_->OnMessageFinalized(message_);
  }

  Message* message_;
  MessageFinalizationListener* listener_ = nullptr;
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_MESSAGE_HANDLE_H_
