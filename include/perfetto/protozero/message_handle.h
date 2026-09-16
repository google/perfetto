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

// Tells whether the message type T is always used as a root message, i.e.
// whether every MessageHandle<T> points to a RootMessage<T>. If so,
// MessageHandle<T> finalizes the message via RootMessage::Finalize() rather
// than Message::Finalize().
//
// protozero doesn't know about specific message types: the layer that owns the
// root-ness of a type specializes this (e.g., perfetto/tracing/
// trace_writer_base.h does that for TracePacket).
//
// The specialization must be visible wherever MessageHandle<T> is named.
// Because root-ness is part of the MessageHandle type (see below), a mismatch
// is diagnosed:
// - Naming MessageHandle<T> before the specialization, in the same translation
//   unit, is a compile error ("explicit specialization after instantiation").
// - Different translation units that disagree on the root-ness of T end up
//   with different types, hence link errors on the functions that take or
//   return MessageHandle<T>.
// Inline code in headers that don't include the specialization is NOT
// diagnosed: such headers must include it.
template <typename T>
struct IsRootMessage : std::false_type {};

// MessageHandle allows to decouple the lifetime of a proto message from the
// underlying storage: the message is finalized via Message::Finalize() (or
// RootMessage::Finalize(), see IsRootMessage) when the handle goes out of scope
// or is assigned a different message. Finalizing the message directly doesn't
// invalidate the handle: Finalize() is idempotent.
//
// |kIsRoot| is not meant to be passed explicitly. It's a template argument
// (rather than a lookup of IsRootMessage<T> in the destructor) so that the
// root-ness is part of the type, see IsRootMessage.
template <typename T, bool kIsRoot = IsRootMessage<T>::value>
class MessageHandle {
 public:
  static constexpr bool kIsRootMessage = kIsRoot;

  MessageHandle() : MessageHandle(nullptr) {}
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

  // See Message::TakeStreamWriter(). The handle won't finalize the message
  // anymore.
  ScatteredStreamWriter* TakeStreamWriter() {
    ScatteredStreamWriter* stream_writer = message_->TakeStreamWriter();
    message_ = nullptr;
    listener_ = nullptr;
    return stream_writer;
  }

 private:
  // For root messages, the RootMessage<T> is finalized as a
  // RootMessage<Message>. This is fine because Message subclasses never add
  // state (see Message::BeginNestedMessage()), so they have the same layout.
  using FinalizeAs =
      typename std::conditional<kIsRoot, RootMessage<Message>, Message>::type;

  void Move(MessageHandle&& other) {
    message_ = other.message_;
    other.message_ = nullptr;
    listener_ = other.listener_;
    other.listener_ = nullptr;
  }

  void FinalizeMessage() {
    static_cast<FinalizeAs*>(message_)->Finalize();
    if (listener_)
      listener_->OnMessageFinalized(message_);
  }

  Message* message_;
  MessageFinalizationListener* listener_ = nullptr;
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_MESSAGE_HANDLE_H_
