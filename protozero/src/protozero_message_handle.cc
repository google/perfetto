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

#include "protozero/protozero_message_handle.h"

#include <utility>

#include "cpp_common/base.h"
#include "protozero/protozero_message.h"

namespace protozero {

namespace {
inline void FinalizeMessageIfSet(ProtoZeroMessage* message) {
  if (message) {
    message->Finalize();
#if PROTOZERO_ENABLE_HANDLE_DEBUGGING()
    message->set_handle(nullptr);
#endif
  }
}
}  // namespace

ProtoZeroMessageHandleBase::ProtoZeroMessageHandleBase(
    ProtoZeroMessage* message)
    : message_(message) {
#if PROTOZERO_ENABLE_HANDLE_DEBUGGING()
  message_->set_handle(this);
#endif
}

ProtoZeroMessageHandleBase::~ProtoZeroMessageHandleBase() {
  FinalizeMessageIfSet(message_);
}

ProtoZeroMessageHandleBase::ProtoZeroMessageHandleBase(
    ProtoZeroMessageHandleBase&& other) noexcept {
  Move(std::move(other));
}

ProtoZeroMessageHandleBase& ProtoZeroMessageHandleBase::operator=(
    ProtoZeroMessageHandleBase&& other) {
  // If the current handle was pointing to a message and is being reset to a new
  // one, finalize the old message.
  FinalizeMessageIfSet(message_);

  Move(std::move(other));
  return *this;
}

void ProtoZeroMessageHandleBase::Move(ProtoZeroMessageHandleBase&& other) {
  // In theory other->message_ could be nullptr, if |other| is a handle that has
  // been std::move-d (and hence empty). There isn't a legitimate use case for
  // doing so, though. Therefore this case is deliberately ignored (if hit, it
  // will manifest as a segfault when dereferencing |message_| below) to avoid a
  // useless null-check.
  message_ = other.message_;
  other.message_ = nullptr;
#if PROTOZERO_ENABLE_HANDLE_DEBUGGING()
  message_->set_handle(this);
#endif
}

}  // namespace protozero
