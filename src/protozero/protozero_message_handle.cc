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

#include "perfetto/protozero/protozero_message_handle.h"

#include <utility>

#include "perfetto/protozero/protozero_message.h"

namespace protozero {

ProtoZeroMessageHandleBase::ProtoZeroMessageHandleBase(
    ProtoZeroMessage* message)
    : message_(message) {
#if PERFETTO_DCHECK_IS_ON()
  generation_ = message_ ? message->generation_ : 0;
  if (message_)
    message_->set_handle(this);
#endif
}

ProtoZeroMessageHandleBase::~ProtoZeroMessageHandleBase() {
  if (message_) {
#if PERFETTO_DCHECK_IS_ON()
    PERFETTO_DCHECK(generation_ == message_->generation_);
#endif
    message_->Finalize();
  }
}

ProtoZeroMessageHandleBase::ProtoZeroMessageHandleBase(
    ProtoZeroMessageHandleBase&& other) noexcept {
  Move(std::move(other));
}

ProtoZeroMessageHandleBase& ProtoZeroMessageHandleBase::operator=(
    ProtoZeroMessageHandleBase&& other) {
  // If the current handle was pointing to a message and is being reset to a new
  // one, finalize the old message. However, if the other message is the same as
  // the one we point to, don't finalize.
  if (message_ && message_ != other.message_)
    message_->Finalize();
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
#if PERFETTO_DCHECK_IS_ON()
  generation_ = message_->generation_;
  message_->set_handle(this);
#endif
}

}  // namespace protozero
