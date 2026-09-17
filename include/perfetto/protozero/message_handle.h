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

#include <functional>
#include <type_traits>
#include <utility>

#include "perfetto/base/export.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_stream_writer.h"

namespace protozero {

class Message;

class PERFETTO_EXPORT_COMPONENT MessageFinalizationListener {
 public:
  virtual ~MessageFinalizationListener();
  virtual void OnMessageFinalized(Message* message) = 0;
};

// When true, MessageHandle<T> uses root-specific functions (currently
// Finalize()), which requires every non-empty handle to refer to a
// RootMessage<T> object.
//
// The specialization must be visible wherever MessageHandle<T> is used, so
// include perfetto/tracing/trace_writer_base.h for TracePacket handles.
template <typename T>
struct IsRootMessage : std::false_type {};

// MessageHandle manages finalization without owning the underlying message:
// - The message is finalized when the handle goes out of scope or is assigned
//   a different message.
// - Calling Message::Finalize() directly leaves the handle intact. The handle
//   still refers to the message and finalizes it again when it goes out of
//   scope. Message::Finalize() is idempotent.
// - Clear or destroy the handle before the message is reset or its storage is
//   released so that finalization cannot affect a new message at the same
//   address or access freed memory, as the handle uses its stored pointer to
//   finalize the message when it goes out of scope.
template <typename T>
class MessageHandle {
 public:
  static constexpr bool kIsRootMessage = IsRootMessage<T>::value;

  MessageHandle() : MessageHandle(nullptr) {}

  // Creates a handle from |message|:
  // - nullptr creates an empty handle.
  // - If IsRootMessage<T> is true, the object must be a RootMessage<T>.
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
    if constexpr (kIsRootMessage) {
      static_cast<RootMessage<T>*>(message_)->Finalize();
    } else {
      message_->Finalize();
    }
    if (listener_)
      listener_->OnMessageFinalized(message_);
  }

  Message* message_;
  MessageFinalizationListener* listener_ = nullptr;
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_MESSAGE_HANDLE_H_
